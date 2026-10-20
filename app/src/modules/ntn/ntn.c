/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/app_version.h>
#include <modem/lte_lc.h>
#include <modem/ntn.h>
#include <modem/nrf_modem_lib.h>
#include <modem/at_monitor.h>
#include <nrf_modem_at.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>

#include "app_common.h"
#include "ntn.h"
#include "network.h"
#include "location.h"

/* Register log module. "ntn" is used by the NTN library. */
LOG_MODULE_REGISTER(ntn_module, CONFIG_APP_NTN_LOG_LEVEL);

BUILD_ASSERT(CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS >
	     CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS,
	     "Watchdog timeout must be greater than maximum message processing time");

/* Cellular profile used by the network module for the terrestrial network. Profile 0 owns
 * PDN contexts 0-9, including the default context used by the rest of the application.
 */
#define TN_PROFILE_ID		0
#define NTN_PROFILE_ID		1

/* Iridium access technology bit in AT%CELLULARPRFL, not part of enum lte_lc_act */
#define NTN_PROFILE_ACT_IRIDIUM	BIT(3)

/* Register zbus subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn_subscriber);

/* Define the channels that the module subscribes to, their associated message types
 * and the subscriber that will receive the messages on the channel.
 */
#define CHANNEL_LIST(X)					\
	X(network_chan,		struct network_msg)	\
	X(location_chan,	struct location_msg)	\
	X(ntn_chan,		struct ntn_msg)

/* Private messages from callbacks and timers to the state machine */
enum priv_ntn_msg_type {
	/* The modem is registered to the NTN network */
	PRIV_NTN_REGISTERED,
	/* The default PDN connection is active, or has been resumed */
	PRIV_NTN_PDN_UP,
	/* The default PDN connection is no longer active */
	PRIV_NTN_PDN_DOWN,
	/* The modem requests a location update for NTN */
	PRIV_NTN_LOCATION_REQUEST,
	/* No terrestrial network connection within CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS */
	PRIV_NTN_TN_TIMEOUT,
	/* Time to search for a terrestrial network again */
	PRIV_NTN_TN_RETRY,
	/* No NTN registration and PDN connection within the connection timeout */
	PRIV_NTN_CONNECTION_TIMEOUT,
	/* CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS elapsed since the last NTN uplink */
	PRIV_NTN_UPLINK_DUE,
};

struct priv_ntn_msg {
	enum priv_ntn_msg_type type;
};

#define PRIV_CHANNEL_LIST(X) \
	X(priv_ntn_chan,	struct priv_ntn_msg)

/* Calculate the maximum message size from the list of channels */
#define MAX_MSG_SIZE	MAX(MAX_MSG_SIZE_FROM_LIST(CHANNEL_LIST), \
			    MAX_MSG_SIZE_FROM_LIST(PRIV_CHANNEL_LIST))

/* Add the ntn_subscriber as observer to all the channels in the list. */
#define ADD_OBSERVERS(_chan, _type)	ZBUS_CHAN_ADD_OBS(_chan, ntn_subscriber, 0);

CHANNEL_LIST(ADD_OBSERVERS)

ZBUS_CHAN_DEFINE(ntn_chan,
		 struct ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(priv_ntn_chan,
		 struct priv_ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(ntn_subscriber),
		 ZBUS_MSG_INIT(0)
);

/* Set while the module waits for NTN registration and PDN connection. Modem events are only
 * forwarded to the state machine while set.
 */
static atomic_t ntn_connecting;

static void tn_timeout_work_fn(struct k_work *work);
static void tn_retry_work_fn(struct k_work *work);
static void connection_timeout_work_fn(struct k_work *work);
static void uplink_interval_work_fn(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(tn_timeout_work, tn_timeout_work_fn);
static K_WORK_DELAYABLE_DEFINE(tn_retry_work, tn_retry_work_fn);
static K_WORK_DELAYABLE_DEFINE(connection_timeout_work, connection_timeout_work_fn);
static K_WORK_DELAYABLE_DEFINE(uplink_interval_work, uplink_interval_work_fn);

static void cereg_mon(const char *notif);

/* lte_lc does not report an unchanged registration status. When the modem resumes from
 * LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG on the same cell, the registration is only visible in the
 * +CEREG notification itself.
 */
AT_MONITOR(ntn_cereg_monitor, "+CEREG", cereg_mon, PAUSED);

/* State machine */

/* NTN module states */
enum ntn_module_state {
	/* The module is running */
	STATE_RUNNING,
		/* The network module owns the modem and uses the terrestrial network */
		STATE_TN,
		/* Waiting for the network module to release the modem */
		STATE_SUSPENDING,
		/* The module owns the modem and uses NTN */
		STATE_NTN,
			/* CFUN=45 between uplinks, or GNSS active while waiting for a fix */
			STATE_NTN_IDLE,
			/* The modem is connecting to NTN to send a location */
			STATE_NTN_CONNECTING,
};

/* State object.
 * Used to transfer context data between state changes.
 */
struct ntn_state_object {
	/* This must be first */
	struct smf_ctx ctx;

	/* Last channel type that a message was received on */
	const struct zbus_channel *chan;

	/* Last received message */
	uint8_t msg_buf[MAX_MSG_SIZE];

	/* Last terrestrial network connection status reported by the network module */
	bool tn_connected;

	/* Last known location */
	struct ntn_location location;

	/* Sample timestamp of the location reported to nRF Cloud */
	int64_t location_timestamp;

	/* Uptime when the location was stored, 0 if no location is available */
	int64_t location_uptime;

	/* Uptime of the last location sent over NTN, 0 if none has been sent */
	int64_t last_uplink_uptime;

	/* Send the next available location regardless of the uplink interval */
	bool uplink_requested;

	/* Return to the terrestrial network when the ongoing NTN uplink is done */
	bool tn_retry_pending;

	/* NTN connection progress */
	bool registered;
	bool pdn_up;

	/* Terrestrial network system mode, restored when handing the modem back */
	enum lte_lc_system_mode tn_system_mode;
	enum lte_lc_system_mode_preference tn_system_mode_pref;
};

/* Forward declarations of state handlers */
static void state_running_entry(void *obj);
static enum smf_state_result state_running_run(void *obj);
static void state_tn_entry(void *obj);
static enum smf_state_result state_tn_run(void *obj);
static void state_tn_exit(void *obj);
static void state_suspending_entry(void *obj);
static enum smf_state_result state_suspending_run(void *obj);
static void state_ntn_entry(void *obj);
static enum smf_state_result state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);
static void state_ntn_idle_entry(void *obj);
static enum smf_state_result state_ntn_idle_run(void *obj);
static void state_ntn_connecting_entry(void *obj);
static enum smf_state_result state_ntn_connecting_run(void *obj);
static void state_ntn_connecting_exit(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL, /* No parent state */
				 &states[STATE_TN]),
	[STATE_TN] =
		SMF_CREATE_STATE(state_tn_entry, state_tn_run, state_tn_exit,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_SUSPENDING] =
		SMF_CREATE_STATE(state_suspending_entry, state_suspending_run, NULL,
				 &states[STATE_RUNNING],
				 NULL),
	[STATE_NTN] =
		SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				 &states[STATE_RUNNING],
				 &states[STATE_NTN_IDLE]),
	[STATE_NTN_IDLE] =
		SMF_CREATE_STATE(state_ntn_idle_entry, state_ntn_idle_run, NULL,
				 &states[STATE_NTN],
				 NULL),
	[STATE_NTN_CONNECTING] =
		SMF_CREATE_STATE(state_ntn_connecting_entry, state_ntn_connecting_run,
				 state_ntn_connecting_exit,
				 &states[STATE_NTN],
				 NULL),
};

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("Watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));

	SEND_FATAL_ERROR_WATCHDOG_TIMEOUT();
}

/* Callbacks and timers */

static void priv_msg_send(enum priv_ntn_msg_type type)
{
	int err;
	const struct priv_ntn_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&priv_ntn_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
	}
}

static void tn_timeout_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_TN_TIMEOUT);
}

static void tn_retry_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_TN_RETRY);
}

static void connection_timeout_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_CONNECTION_TIMEOUT);
}

static void uplink_interval_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	priv_msg_send(PRIV_NTN_UPLINK_DUE);
}

static void uplink_interval_schedule(const struct ntn_state_object *state)
{
	int64_t elapsed_ms;
	int64_t interval_ms;
	int64_t delay_ms;

	elapsed_ms = k_uptime_get() - state->last_uplink_uptime;
	interval_ms = (int64_t)CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS * MSEC_PER_SEC;
	delay_ms = interval_ms - elapsed_ms;

	if (delay_ms < 0) {
		delay_ms = 0;
	}

	(void)k_work_reschedule(&uplink_interval_work, K_MSEC(delay_ms));
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (!atomic_get(&ntn_connecting)) {
		return;
	}

	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
		    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			priv_msg_send(PRIV_NTN_REGISTERED);
		}

		break;
	case LTE_LC_EVT_PDN:
		switch (evt->pdn.type) {
		case LTE_LC_EVT_PDN_ACTIVATED: __fallthrough;
		case LTE_LC_EVT_PDN_RESUMED:
			LOG_DBG("PDN up, cid: %d", evt->pdn.cid);
			priv_msg_send(PRIV_NTN_PDN_UP);

			break;
		case LTE_LC_EVT_PDN_DEACTIVATED: __fallthrough;
		case LTE_LC_EVT_PDN_NETWORK_DETACH: __fallthrough;
		case LTE_LC_EVT_PDN_SUSPENDED:
			LOG_DBG("PDN down, cid: %d", evt->pdn.cid);
			priv_msg_send(PRIV_NTN_PDN_DOWN);

			break;
		default:
			break;
		}

		break;
	default:
		break;
	}
}

static void cereg_mon(const char *notif)
{
	const char *p = strchr(notif, ':');
	int status;

	if (p == NULL) {
		return;
	}

	p++;

	while (*p == ' ') {
		p++;
	}

	if (*p < '0' || *p > '9') {
		return;
	}

	status = atoi(p);

	if ((status == LTE_LC_NW_REG_REGISTERED_HOME) ||
	    (status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
		priv_msg_send(PRIV_NTN_REGISTERED);
	}
}

static void ntn_evt_handler(const struct ntn_evt *evt)
{
	if ((evt->type == NTN_EVT_LOCATION_REQUEST) && evt->location_request.requested) {
		LOG_DBG("Location requested by the modem, accuracy: %d m",
			evt->location_request.accuracy);

		priv_msg_send(PRIV_NTN_LOCATION_REQUEST);
	}
}

/* Configure both cellular profiles. Required before CFUN=45 (OFFLINE_KEEP_REG).
 * Must be done while the modem is in CFUN=0 or CFUN=4, for example in NRF_MODEM_LIB_ON_INIT.
 */
static int cellular_profiles_configure(void)
{
	int err;
	struct lte_lc_cellular_profile tn_profile = {
		.id = TN_PROFILE_ID,
		.act = LTE_LC_ACT_LTEM | LTE_LC_ACT_NBIOT,
		.uicc = LTE_LC_UICC_PHYSICAL,
	};

	err = lte_lc_cellular_profile_configure(&tn_profile);
	if (err) {
		LOG_ERR("Failed to configure TN cellular profile, error: %d", err);

		return err;
	}

	err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,%d,%d,%d",
				  NTN_PROFILE_ID, (int)NTN_PROFILE_ACT_IRIDIUM, LTE_LC_UICC_PHYSICAL);
	if (err) {
		LOG_ERR("Failed to configure Iridium NTN cellular profile, error: %d", err);

		return err;
	}

	LOG_INF("Cellular profiles configured (TN: profile %d, NTN: profile %d)",
		TN_PROFILE_ID, NTN_PROFILE_ID);

	return 0;
}

static void on_modem_init(int ret, void *ctx)
{
	ARG_UNUSED(ctx);

	if (ret) {
		return;
	}

	(void)cellular_profiles_configure();
}

NRF_MODEM_LIB_ON_INIT(ntn_modem_init_hook, on_modem_init, NULL);

/* Helpers */

static void ntn_status_notify(enum ntn_msg_type type)
{
	int err;
	const struct ntn_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&ntn_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void network_request_send(enum network_msg_type type)
{
	int err;
	const struct network_msg msg = {
		.type = type,
	};

	err = zbus_chan_pub(&network_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static void gnss_search_trigger(void)
{
	int err;
	static const struct location_msg msg = {
		.type = LOCATION_GNSS_SEARCH_TRIGGER,
	};

	LOG_DBG("Requesting GNSS location");

	err = zbus_chan_pub(&location_chan, &msg, PUB_TIMEOUT);
	if (err) {
		LOG_ERR("zbus_chan_pub, error: %d", err);
		SEND_FATAL_ERROR();
	}
}

static bool location_is_valid(const struct ntn_state_object *state)
{
	if (state->location_uptime == 0) {
		return false;
	}

	if (CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS == 0) {
		return true;
	}

	return (k_uptime_get() - state->location_uptime) <
	       ((int64_t)CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS * MSEC_PER_SEC);
}

/* Remaining location validity in seconds as expected by ntn_location_set(), 0 is infinite */
static uint32_t location_validity_left(const struct ntn_state_object *state)
{
	int64_t left_ms;

	if (CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS == 0) {
		return 0;
	}

	left_ms = ((int64_t)CONFIG_APP_NTN_LOCATION_VALIDITY_SECONDS * MSEC_PER_SEC) -
		  (k_uptime_get() - state->location_uptime);

	return (uint32_t)MAX(1, left_ms / MSEC_PER_SEC);
}

static bool uplink_is_due(const struct ntn_state_object *state)
{
	if (state->uplink_requested || (state->last_uplink_uptime == 0)) {
		return true;
	}

	return (k_uptime_get() - state->last_uplink_uptime) >=
	       ((int64_t)CONFIG_APP_NTN_UPLINK_INTERVAL_SECONDS * MSEC_PER_SEC);
}

static void location_store(struct ntn_state_object *state, const struct ntn_location *location,
			   int64_t timestamp)
{
	state->location = *location;
	state->location_timestamp = timestamp;
	state->location_uptime = k_uptime_get();

	LOG_DBG("Location stored: lat: %f, lon: %f, alt: %f, acc: %f",
		state->location.latitude, state->location.longitude,
		(double)state->location.altitude, (double)state->location.accuracy);
}

static void location_store_from_gnss(struct ntn_state_object *state,
				     const struct location_msg *msg)
{
	const struct ntn_location location = {
		.latitude = msg->gnss_data.latitude,
		.longitude = msg->gnss_data.longitude,
		.altitude = msg->gnss_data.details.gnss.pvt_data.altitude,
		.accuracy = msg->gnss_data.accuracy,
	};

	location_store(state, &location, msg->timestamp);
}

static int set_ntn_offline_mode(void)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	if ((mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG) ||
	    (mode == LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON)) {
		return 0;
	}

	LOG_DBG("Setting CFUN=45 after NTN session (was %d)", mode);

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);
	}

	return err;
}

/* tn_ntn_usecase set_gnss_active_mode */
static int gnss_mode_enter(void)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	LOG_DBG("GNSS mode enter, current CFUN: %d", mode);

	if (mode != LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG) {
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);

			return err;
		}
	}

	err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set(GPS), error: %d", err);

		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_GNSS), error: %d", err);

		return err;
	}

	return 0;
}

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
static int channel_select_set(void)
{
	int err;

	err = nrf_modem_at_printf("AT%%FREQRANGES=1,7,,1,\"%d\",\"\"",
				  CONFIG_APP_NTN_CHANNEL_SELECT);
	if (err) {
		LOG_ERR("Failed to select NTN channel %d, error: %d",
			CONFIG_APP_NTN_CHANNEL_SELECT, err);
	}

	return err;
}
#endif /* CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE */

static int periodic_search_set(void)
{
	int err;
	const struct lte_lc_periodic_search_cfg search_cfg = {
		.pattern_count = 1,
		.loop = true,
		.return_to_pattern = 0,
		.band_optimization = 0,
		.patterns[0] = {
			.type = LTE_LC_PERIODIC_SEARCH_PATTERN_TABLE,
			.table = {
				.val_1 = 2,
				.val_2 = -1,
				.val_3 = -1,
				.val_4 = -1,
				.val_5 = -1,
			},
		},
	};

	err = lte_lc_periodic_search_set(&search_cfg);
	if (err) {
		LOG_ERR("lte_lc_periodic_search_set, error: %d", err);
	}

	return err;
}

static int system_mode_ntn_set(void)
{
	int err;

	/* Iridium NTN TDD NB-IoT is not supported by lte_lc. Omitted parameters default to 0,
	 * so lte_lc_system_mode_set() clears this mode again.
	 */
	err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,0,1");
	if (err) {
		LOG_ERR("Failed to set NTN system mode, error: %d", err);
	}

	return err;
}

/* Switch to the NTN cellular profile and start connecting (tn_ntn_usecase set_ntn_active_mode) */
static int ntn_mode_enter(const struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return err;
	}

	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG:
		break;
	case LTE_LC_FUNC_MODE_OFFLINE:
	case LTE_LC_FUNC_MODE_POWER_OFF:
		break;
	default:
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);

			return err;
		}

		break;
	}

	err = system_mode_ntn_set();
	if (err) {
		return err;
	}

	err = ntn_location_set(state->location.latitude, state->location.longitude,
			       state->location.altitude, location_validity_left(state));
	if (err) {
		LOG_ERR("ntn_location_set, error: %d", err);

		return err;
	}

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%d\"", CONFIG_APP_NTN_BANDLOCK);
	if (err) {
		LOG_ERR("Failed to lock NTN band %d, error: %d", CONFIG_APP_NTN_BANDLOCK, err);

		return err;
	}
#endif /* CONFIG_APP_NTN_BANDLOCK_ENABLE */

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = channel_select_set();
	if (err) {
		return err;
	}
#endif /* CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE */

	err = periodic_search_set();
	if (err) {
		return err;
	}

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_LTE), error: %d", err);

		return err;
	}

	return 0;
}

/* Same sequence as tn_ntn_usecase state_tn_entry */
static void tn_mode_restore(const struct ntn_state_object *state)
{
	int err;
	enum lte_lc_func_mode mode;

	err = lte_lc_func_mode_get(&mode);
	if (err) {
		LOG_ERR("lte_lc_func_mode_get, error: %d", err);

		return;
	}

	LOG_DBG("TN restore, current CFUN: %d", mode);

	switch (mode) {
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG:
	case LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG_UICC_ON:
		break;
	case LTE_LC_FUNC_MODE_OFFLINE:
	case LTE_LC_FUNC_MODE_POWER_OFF:
		break;
	default:
		/* tn_ntn_usecase uses CFUN=0 here; with CONFIG_LTE_LC_PDN_DEFAULTS_OVERRIDE that
		 * makes lte_lc re-run AT+CGDCONT on CID 0 via the modem CFUN hook, which fails
		 * while NTN XSYSTEMMODE is still active. Use CFUN=45 like after an NTN session.
		 */
		err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_KEEP_REG);
		if (err) {
			LOG_ERR("lte_lc_func_mode_set(OFFLINE_KEEP_REG), error: %d", err);

			return;
		}

		break;
	}

#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
	err = nrf_modem_at_printf("AT%%XBANDLOCK=0");
	if (err) {
		LOG_ERR("Failed to remove band lock, error: %d", err);

		return;
	}
#endif /* CONFIG_APP_NTN_BANDLOCK_ENABLE */

#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
	err = nrf_modem_at_printf("AT%%FREQRANGES=0");
	if (err) {
		LOG_ERR("Failed to clear frequency ranges, error: %d", err);
	}
#endif /* CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE */

	err = lte_lc_periodic_search_clear();
	if (err) {
		LOG_ERR("lte_lc_periodic_search_clear, error: %d", err);
	}

	err = lte_lc_system_mode_set(state->tn_system_mode, state->tn_system_mode_pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_set, error: %d", err);

		return;
	}

	/* lte_lc_connect_async() skips CFUN changes when already registered, so leave CFUN=45
	 * explicitly when handing the modem back to the network module.
	 */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err) {
		LOG_ERR("lte_lc_func_mode_set(ACTIVATE_LTE), error: %d", err);
	}
}

/* Send the last known location to nRF Cloud. The CoAP session is paused by the cloud module
 * while the network module is suspended, so it can be resumed here without a new DTLS
 * handshake. It is paused again afterwards, so that the cloud module can resume it once the
 * terrestrial network is available.
 */
static int cloud_location_send(const struct ntn_state_object *state)
{
	int err;
	int pause_err;
	const struct nrf_cloud_gnss_data gnss_data = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = state->location_timestamp,
		.pvt = {
			.lat = state->location.latitude,
			.lon = state->location.longitude,
			.accuracy = state->location.accuracy,
			.alt = state->location.altitude,
			.has_alt = 1,
		},
	};

	err = nrf_cloud_coap_resume();
	if (err) {
		LOG_WRN("nrf_cloud_coap_resume, error: %d", err);

		if (!IS_ENABLED(CONFIG_APP_NTN_CLOUD_FULL_CONNECT)) {
			return err;
		}

		LOG_INF("Connecting to nRF Cloud over NTN");

		err = nrf_cloud_coap_connect(APP_VERSION_STRING);
		if (err) {
			LOG_ERR("nrf_cloud_coap_connect, error: %d", err);

			return err;
		}
	}

	/* Confirmable, so that the location has been delivered before the session is saved */
	err = nrf_cloud_coap_location_send(&gnss_data, true);
	if (err) {
		LOG_ERR("nrf_cloud_coap_location_send, error: %d", err);
	} else {
		LOG_INF("Location sent to nRF Cloud over NTN");

#if defined(CONFIG_NRF_CLOUD_SEND_SHADOW_INFO)
		/* networkInfo, simInfo, connectionInfo, and other enabled shadow sections */
		int shadow_err = nrf_cloud_coap_shadow_configured_info_update(APP_VERSION_STRING);

		if (shadow_err) {
			LOG_WRN("nrf_cloud_coap_shadow_configured_info_update, error: %d", shadow_err);
		} else {
			LOG_DBG("Device shadow info sections sent over NTN");
		}
#endif /* CONFIG_NRF_CLOUD_SEND_SHADOW_INFO */
	}

	pause_err = nrf_cloud_coap_pause();
	if (pause_err) {
		LOG_WRN("nrf_cloud_coap_pause, error: %d", pause_err);
	}

	return err;
}

static void uplink_try(struct ntn_state_object *state)
{
	int err;

	if (!state->registered || !state->pdn_up) {
		return;
	}

	err = cloud_location_send(state);

	state->last_uplink_uptime = k_uptime_get();

	ntn_status_notify(err ? NTN_UPLINK_FAILED : NTN_UPLINK_SENT);

	if (!err) {
		/* tn_ntn_usecase state_ntn_exit: leave NTN LTE before GNSS idle */
		(void)set_ntn_offline_mode();
	}

	smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);
}

/* State handlers */

static void state_running_entry(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	lte_lc_register_handler(lte_lc_evt_handler);
	ntn_register_handler(ntn_evt_handler);
}

static enum smf_state_result state_running_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state->msg_buf;

		if (msg->type == LOCATION_GNSS_DATA) {
			location_store_from_gnss(state, msg);
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SET) {
			location_store(state, &msg->location, NRF_CLOUD_NO_TIMESTAMP);
		}
	}

	return SMF_EVENT_HANDLED;
}

static void state_tn_entry(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	if (!state->tn_connected) {
		(void)k_work_reschedule(&tn_timeout_work,
					K_SECONDS(CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS));
	}
}

static enum smf_state_result state_tn_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		switch (msg->type) {
		case NETWORK_CONNECTED:
			(void)k_work_cancel_delayable(&tn_timeout_work);

			return SMF_EVENT_HANDLED;
		case NETWORK_DISCONNECTED:
			/* Keeps the original deadline if the timer is already running */
			(void)k_work_schedule(&tn_timeout_work,
					      K_SECONDS(CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS));

			return SMF_EVENT_HANDLED;
		case NETWORK_SEARCH_DONE:
			if (IS_ENABLED(CONFIG_APP_NTN_FALLBACK_ON_SEARCH_DONE) &&
			    !state->tn_connected) {
				LOG_INF("No terrestrial network found, falling back to NTN");
				smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);
			}

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		if ((msg->type == PRIV_NTN_TN_TIMEOUT) && !state->tn_connected) {
			LOG_INF("No terrestrial network for %d s, falling back to NTN",
				CONFIG_APP_NTN_TN_FALLBACK_TIMEOUT_SECONDS);
			smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);

			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_FALLBACK_START_REQUEST) {
			smf_set_state(SMF_CTX(state), &states[STATE_SUSPENDING]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_tn_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&tn_timeout_work);
}

static void state_suspending_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	err = lte_lc_system_mode_get(&state->tn_system_mode, &state->tn_system_mode_pref);
	if (err) {
		LOG_ERR("lte_lc_system_mode_get, error: %d", err);

		state->tn_system_mode = LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS;
		state->tn_system_mode_pref = LTE_LC_SYSTEM_MODE_PREFER_AUTO;
	}

	network_request_send(NETWORK_SUSPEND);
}

static enum smf_state_result state_suspending_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_SUSPENDED) {
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_entry(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	state->tn_retry_pending = false;
	state->uplink_requested = true;

	(void)k_work_reschedule(&tn_retry_work, K_MINUTES(CONFIG_APP_NTN_TN_RETRY_INTERVAL_MINUTES));

	ntn_status_notify(NTN_FALLBACK_STARTED);
}

static enum smf_state_result state_ntn_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		if (msg->type == PRIV_NTN_TN_RETRY) {
			state->tn_retry_pending = true;

			return SMF_EVENT_HANDLED;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_FALLBACK_STOP_REQUEST) {
			state->tn_retry_pending = true;

			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_exit(void *obj)
{
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&tn_retry_work);
	(void)k_work_cancel_delayable(&uplink_interval_work);

	tn_mode_restore(state);

	LOG_INF("Searching for a terrestrial network");

	network_request_send(NETWORK_RESUME);
	ntn_status_notify(NTN_FALLBACK_STOPPED);
}

static void state_ntn_idle_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	if (state->tn_retry_pending) {
		smf_set_state(SMF_CTX(state), &states[STATE_TN]);

		return;
	}

	if (state->uplink_requested && location_is_valid(state)) {
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);

		return;
	}

	if (uplink_is_due(state) && location_is_valid(state)) {
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);

		return;
	}

	/* tn_ntn_usecase STATE_IDLE: stay at CFUN=45 until the next uplink or stale location */
	if (location_is_valid(state)) {
		LOG_DBG("Location valid, NTN idle at CFUN=45 until next uplink");
		uplink_interval_schedule(state);

		return;
	}

	err = gnss_mode_enter();
	if (err) {
		LOG_ERR("Failed to enable GNSS, error: %d", err);
	}

	gnss_search_trigger();
}

static enum smf_state_result state_ntn_idle_run(void *obj)
{
	struct ntn_state_object *state = obj;

	if (state->chan == &location_chan) {
		const struct location_msg *msg = (const struct location_msg *)state->msg_buf;

		if (msg->type != LOCATION_GNSS_DATA) {
			return SMF_EVENT_HANDLED;
		}

		location_store_from_gnss(state, msg);

		if (uplink_is_due(state)) {
			smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
		}

		return SMF_EVENT_HANDLED;
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case NTN_UPLINK_REQUEST:
			state->uplink_requested = true;

			if (location_is_valid(state)) {
				smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
			} else {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		case NTN_FALLBACK_STOP_REQUEST:
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case PRIV_NTN_TN_RETRY:
			smf_set_state(SMF_CTX(state), &states[STATE_TN]);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_LOCATION_REQUEST:
			if (!location_is_valid(state)) {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_UPLINK_DUE:
			if (location_is_valid(state)) {
				smf_set_state(SMF_CTX(state), &states[STATE_NTN_CONNECTING]);
			} else {
				gnss_search_trigger();
			}

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_connecting_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&uplink_interval_work);

	state->uplink_requested = false;
	state->registered = false;
	state->pdn_up = false;

	atomic_set(&ntn_connecting, 1);
	at_monitor_resume(&ntn_cereg_monitor);

	err = ntn_mode_enter(state);
	if (err) {
		LOG_ERR("Failed to connect to NTN, error: %d", err);

		ntn_status_notify(NTN_UPLINK_FAILED);
		smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);

		return;
	}

	(void)k_work_reschedule(&connection_timeout_work,
				K_SECONDS(CONFIG_APP_NTN_CONNECTION_TIMEOUT_SECONDS));
}

static enum smf_state_result state_ntn_connecting_run(void *obj)
{
	int err;
	struct ntn_state_object *state = obj;

	if (state->chan == &priv_ntn_chan) {
		const struct priv_ntn_msg *msg = (const struct priv_ntn_msg *)state->msg_buf;

		switch (msg->type) {
		case PRIV_NTN_REGISTERED:
			if (!state->registered) {
				LOG_INF("Registered to NTN network");

				state->registered = true;

				(void)k_work_reschedule(&connection_timeout_work,
					K_SECONDS(CONFIG_APP_NTN_REGISTERED_TIMEOUT_SECONDS));
			}

			uplink_try(state);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_PDN_UP:
			state->pdn_up = true;

			uplink_try(state);

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_PDN_DOWN:
			state->pdn_up = false;

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_LOCATION_REQUEST:
			if (location_is_valid(state)) {
				err = ntn_location_set(state->location.latitude,
						       state->location.longitude,
						       state->location.altitude,
						       location_validity_left(state));
				if (err) {
					LOG_ERR("ntn_location_set, error: %d", err);
				}
			}

			return SMF_EVENT_HANDLED;
		case PRIV_NTN_CONNECTION_TIMEOUT:
			/* The timeout was extended after the message was queued */
			if (k_work_delayable_is_pending(&connection_timeout_work)) {
				return SMF_EVENT_HANDLED;
			}

			LOG_WRN("NTN connection timed out, registered: %d, PDN up: %d",
				state->registered, state->pdn_up);

			ntn_status_notify(NTN_UPLINK_FAILED);
			smf_set_state(SMF_CTX(state), &states[STATE_NTN_IDLE]);

			return SMF_EVENT_HANDLED;
		default:
			break;
		}
	} else if (state->chan == &ntn_chan) {
		const struct ntn_msg *msg = (const struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_UPLINK_REQUEST) {
			return SMF_EVENT_HANDLED;
		}
	}

	return SMF_EVENT_PROPAGATE;
}

static void state_ntn_connecting_exit(void *obj)
{
	ARG_UNUSED(obj);

	LOG_DBG("%s", __func__);

	(void)k_work_cancel_delayable(&connection_timeout_work);

	at_monitor_pause(&ntn_cereg_monitor);
	atomic_clear(&ntn_connecting);

	/* tn_ntn_usecase state_ntn_exit: CFUN=45 after an NTN session before GNSS idle */
	(void)set_ntn_offline_mode();
}

static void tn_connection_status_retain(struct ntn_state_object *state)
{
	if (state->chan == &network_chan) {
		const struct network_msg *msg = (const struct network_msg *)state->msg_buf;

		if (msg->type == NETWORK_CONNECTED) {
			state->tn_connected = true;
		} else if (msg->type == NETWORK_DISCONNECTED) {
			state->tn_connected = false;
		}
	}
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = (CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const uint32_t execution_time_ms =
		(CONFIG_APP_NTN_MSG_PROCESSING_TIMEOUT_SECONDS * MSEC_PER_SEC);
	const k_timeout_t zbus_wait_ms = K_MSEC(wdt_timeout_ms - execution_time_ms);
	/* Place state object in .data section to ensure it is captured in coredumps
	 * and can be inspected by external tools during state analysis.
	 */
	__attribute__((section(".data"))) static struct ntn_state_object ntn_state;

	LOG_DBG("NTN module task started");

	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		SEND_FATAL_ERROR();
		return;
	}

	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		err = zbus_sub_wait_msg(&ntn_subscriber, &ntn_state.chan, ntn_state.msg_buf,
					zbus_wait_ms);
		if (err == -ENOMSG) {
			continue;
		} else if (err) {
			LOG_ERR("zbus_sub_wait_msg, error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}

		tn_connection_status_retain(&ntn_state);

		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("smf_run_state(), error: %d", err);
			SEND_FATAL_ERROR();
			return;
		}
	}
}

K_THREAD_DEFINE(ntn_module_thread_id,
		CONFIG_APP_NTN_THREAD_STACK_SIZE,
		ntn_module_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
