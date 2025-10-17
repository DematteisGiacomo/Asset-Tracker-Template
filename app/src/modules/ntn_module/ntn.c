/*
 * Copyright (c) 2025 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/smf.h>
#include <modem/lte_lc.h>
#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <nrf_modem_gnss.h>
#include <modem/modem_info.h>
#include <zephyr/task_wdt/task_wdt.h>
#include <zephyr/net/socket.h>
#include <errno.h>

#include "ntn.h"

/* Socket state */
static int sock_fd = -1;
static struct sockaddr_storage host_addr;

LOG_MODULE_REGISTER(ntn, CONFIG_APP_NTN_LOG_LEVEL);

/* Define channels provided by this module */
ZBUS_CHAN_DEFINE(NTN_CHAN,
		 struct ntn_msg,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(0)
);

/* Register subscriber */
ZBUS_MSG_SUBSCRIBER_DEFINE(ntn);

/* Observe NTN channel */
ZBUS_CHAN_ADD_OBS(NTN_CHAN, ntn, 0);

#define MAX_MSG_SIZE sizeof(struct ntn_msg)

/* State machine states */
enum ntn_module_state {
	STATE_RUNNING,
	STATE_GNSS,
	STATE_NTN,
};

/* State object */
struct ntn_state_object {
	struct smf_ctx ctx;
	const struct zbus_channel *chan;
	uint8_t msg_buf[MAX_MSG_SIZE];
	struct k_timer ntn_timer;
	bool socket_connected;
	bool ntn_initialized;
	bool gnss_initialized;
};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static struct k_work timer_work;
static struct k_work gnss_location_work;

/* Forward declarations */

static void gnss_event_handler(int event);
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt);

/* Forward declarations */
static void state_running_entry(void *obj);
static void state_running_run(void *obj);
static void state_gnss_entry(void *obj);
static void state_gnss_run(void *obj);
static void state_gnss_exit(void *obj);
static void state_ntn_entry(void *obj);
static void state_ntn_run(void *obj);
static void state_ntn_exit(void *obj);

/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] = SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				NULL, &states[STATE_GNSS]),
	[STATE_GNSS] = SMF_CREATE_STATE(state_gnss_entry, state_gnss_run, state_gnss_exit,
				&states[STATE_RUNNING], NULL),
	[STATE_NTN] = SMF_CREATE_STATE(state_ntn_entry, state_ntn_run, state_ntn_exit,
				&states[STATE_RUNNING], NULL),
};

/* Helper function to publish NTN messages */
static void ntn_msg_publish(enum ntn_msg_type type)
{
	int err;
	struct ntn_msg msg = {
		.type = type
	};

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN message, error: %d", err);
		return;
	}
}

static void timer_work_handler(struct k_work *work)
{
	ntn_msg_publish(NTN_TIMEOUT);
}

/* Timer callback for NTN mode timeout */
static void ntn_timer_handler(struct k_timer *timer)
{
	k_work_submit(&timer_work);
}

static void apply_gnss_time(const struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	struct tm gnss_time = {
		.tm_year = pvt_data->datetime.year - 1900,
		.tm_mon = pvt_data->datetime.month - 1,
		.tm_mday = pvt_data->datetime.day,
		.tm_hour = pvt_data->datetime.hour,
		.tm_min = pvt_data->datetime.minute,
		.tm_sec = pvt_data->datetime.seconds,
	};

	date_time_set(&gnss_time);
}

static void gnss_location_work_handler(struct k_work *work)
{
    int err;
    struct nrf_modem_gnss_pvt_data_frame pvt_data;

    /* Read PVT data in thread context */
    err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data), NRF_MODEM_GNSS_DATA_PVT);
    if (err != 0) {
        LOG_ERR("Failed to read GNSS data nrf_modem_gnss_read(), err: %d", err);
        return;
    }

    if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
        LOG_DBG("Got valid GNSS location: lat: %f, lon: %f, alt: %f",
		(double)pvt_data.latitude,
		(double)pvt_data.longitude,
		(double)pvt_data.altitude);
        memcpy(&last_pvt, &pvt_data, sizeof(last_pvt));
        apply_gnss_time(&last_pvt);
        ntn_msg_publish(NTN_LOCATION_SEARCH_DONE);

	/* Log SV (Satellite Vehicle) data */
	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt_data.sv[i].sv == 0) {
		/* SV not valid, skip */
		continue;
		}

		LOG_DBG("SV: %3d C/N0: %4.1f el: %2d az: %3d signal: %d in fix: %d unhealthy: %d",
		pvt_data.sv[i].sv,
		pvt_data.sv[i].cn0 * 0.1,
		pvt_data.sv[i].elevation,
		pvt_data.sv[i].azimuth,
		pvt_data.sv[i].signal,
		pvt_data.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX ? 1 : 0,
		pvt_data.sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY ? 1 : 0);
	}
    }
}

static int sgp4_propagator_compute()
{
	// Placeholder
	return CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES * 60;
}

/* Helper functions */

static int set_ntn_dormant_mode(void)
{
	int err;

	/* Set modem to dormant mode without loosing ATTACH  */
	// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_OFFLINE_UICC_ON)
	err = nrf_modem_at_printf("AT+CFUN=45");
	if (err) {
		LOG_ERR("Failed to set AT+CFUN=45, error: %d", err);
		return err;
	}

	return 0;
}

static int set_ntn_active_mode(struct ntn_state_object *state)
{
	int err;

	if (state->ntn_initialized)
	{
		/* Configure NTN system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);
			return err;
		}
		/* Configure location using latest GNSS data */
		err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"%f\",0,0",
					(double)last_pvt.latitude,
					(double)last_pvt.longitude,
					(double)last_pvt.altitude);
		if (err) {
			LOG_ERR("Failed to set AT%%LOCATION, error: %d", err);
			return err;
		}
		LOG_DBG("NTN initialized, using AT+CFUN=21");
		err = nrf_modem_at_printf("AT+CFUN=21");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=21, error: %d", err);
			return err;
		}
	}
	else
	{
		/* Set modem to minimum functionality */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF)
		err = nrf_modem_at_printf("AT+CFUN=0");
		if (err) {
			LOG_ERR("Failed to set modem to minimum functionality, error: %d", err);
			return err;
		}
		/* Set NTN profile */
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,0,4,0");
		if (err) {
			LOG_ERR("Failed to set modem NTN profile, error: %d", err);
			return err;
		}

		/* Set TN profile */
		err = nrf_modem_at_printf("AT%%CELLULARPRFL=2,1,1,0");
		if (err) {
			LOG_ERR("Failed to set modem TN profile, error: %d", err);
			return err;
		}

		#if defined(CONFIG_APP_NTN_DISABLE_EPCO)
		/* Set XEPCO off */
		err = nrf_modem_at_printf("AT%%XEPCO=0");
		if (err) {
			LOG_ERR("Failed to set XEPCO off, error: %d", err);
			return err;
		}
		#endif

		/* Configure NTN system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NTN_NBIOT, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,0,0,1");
		if (err) {
			LOG_ERR("Failed to set NTN system mode, error: %d", err);
			return err;
		}

		/* Configure location using latest GNSS data */
		err = nrf_modem_at_printf("AT%%LOCATION=2,\"%f\",\"%f\",\"%f\",0,0",
					(double)last_pvt.latitude,
					(double)last_pvt.longitude,
					(double)last_pvt.altitude);
		if (err) {
			LOG_ERR("Failed to set AT%%LOCATION, error: %d", err);
			return err;
		}

		#if defined(CONFIG_APP_NTN_BANDLOCK_ENABLE)
			err = nrf_modem_at_printf("AT%%XBANDLOCK=2,,\"%i\"", CONFIG_APP_NTN_BANDLOCK);
			if (err) {
				LOG_ERR("Failed to set NTN band lock, error: %d", err);
				return err;
			}
		#endif

		#if defined(CONFIG_APP_NTN_CHANNEL_SELECT_ENABLE)
			err = nrf_modem_at_printf("AT%%CHSELECT=1,14,%i", CONFIG_APP_NTN_CHANNEL_SELECT);
			if (err) {
				LOG_ERR("Failed to set NTN channel, error: %d", err);
				return err;
			}
		#endif

		#if defined(CONFIG_APP_NTN_APN)
			err = nrf_modem_at_printf("AT+CGDCONT=0,\"ip\",\"%s\"", CONFIG_APP_NTN_APN);
			if (err) {
				LOG_ERR("Failed to set NTN APN, error: %d", err);
				return err;
			}
		#endif

		/*
		Modem is activating AT+CPSMS via CONFIG_LTE_LC_PSM_MODULE=y.
		Cast AT+CPSMS=0 to deactivate legacy PSM.
		CFUN=45 + legacy PSM is not supported, has bugs.
		*/
		err = nrf_modem_at_printf("AT+CPSMS=0");
		if (err) {
			LOG_ERR("Failed to set AT+CPSMS=0, error: %d", err);
			return err;
		}

		state->ntn_initialized=true;

		k_sleep(K_MSEC(5000));

		LOG_DBG("NTN not initialized, using lte_lc_connect_async to connect to network");
		err = lte_lc_connect_async(lte_lc_evt_handler);
		if (err) {
			LOG_ERR("lte_lc_connect_async, error: %d\n", err);
			return;
		}
	}

	return 0;
}

static int set_gnss_active_mode(struct ntn_state_object *state)
{
	int err;

	if (state->gnss_initialized)
	{
		/* Configure GNSS system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
		if (err) {
			LOG_ERR("Failed to set GNSS system mode, error: %d", err);
			return err;
		}

		/* Activate GNSS mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS)
		err = nrf_modem_at_printf("AT+CFUN=31");
		if (err) {
			LOG_ERR("Failed to activate GNSS mode, error: %d", err);
			return err;
		}
	}
	else
	{
		/* Set modem to offline mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_POWER_OFF)
		err = nrf_modem_at_printf("AT+CFUN=0");
		if (err) {
			LOG_ERR("Failed to set AT+CFUN=0, error: %d", err);
			return err;
		}

		/* Configure GNSS system mode */
		// lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_GPS, LTE_LC_SYSTEM_MODE_PREFER_AUTO)
		err = nrf_modem_at_printf("AT%%XSYSTEMMODE=0,0,1,0,0");
		if (err) {
			LOG_ERR("Failed to set GNSS system mode, error: %d", err);
			return err;
		}

		/* Activate GNSS mode */
		// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS)
		err = nrf_modem_at_printf("AT+CFUN=31");
		if (err) {
			LOG_ERR("Failed to activate GNSS mode, error: %d", err);
			return err;
		}
		state->gnss_initialized=true;
	}

	return 0;
}

static int set_gnss_inactive_mode(void)
{
	int err;

	/* Set modem to CFUN=30 mode when exiting GNSS state */
	// lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_GNSS)
	err = nrf_modem_at_printf("AT+CFUN=30");
	if (err) {
		LOG_ERR("Failed to set modem to CFUN=30 mode, error: %d", err);
		return err;
	}
	return 0;
}

/* Socket functions */
static int sock_open_and_connect(void)
{
	int err;
	struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

	server4->sin_family = AF_INET;
	server4->sin_port = htons(CONFIG_APP_NTN_SERVER_PORT);
	
	inet_pton(AF_INET, CONFIG_APP_NTN_SERVER_ADDR, &server4->sin_addr);

	/* Create UDP socket */
	sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		LOG_ERR("Failed to create UDP socket, error: %d", errno);
		return -errno;
	}

	/* Connect socket */
	err = connect(sock_fd, (struct sockaddr *)&host_addr, sizeof(struct sockaddr_in));
	if (err < 0) {
		LOG_ERR("Failed to connect socket, error: %d", errno);
		close(sock_fd);
		sock_fd = -1;
		return -errno;
	}

	return 0;
}

static int sock_send_gnss_data(const struct nrf_modem_gnss_pvt_data_frame *gnss_data, int64_t ping_rtt)
{
	int err;
	char message[256];

	if (sock_fd < 0) {
		LOG_ERR("Socket not connected");
		return -ENOTCONN;
	}

#if defined(CONFIG_APP_NTN_THINGY_ROCKS_ENDPOINT)
	char rsrp[16] = {0}, band[16] = {0}, ue_mode[16] = {0}, oper[16] = {0}, imei[16] = {0};
	char temp[16] = {0};

	err = modem_info_string_get(MODEM_INFO_IMEI, imei, sizeof(imei));
	if (err < 0) {
			snprintf(imei, sizeof(imei), "N/A");
		}
	err = modem_info_string_get(MODEM_INFO_RSRP, rsrp, sizeof(rsrp));
	if (err < 0) {
		snprintf(rsrp, sizeof(rsrp), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_CUR_BAND, band, sizeof(band));
	if (err < 0) {
		snprintf(band, sizeof(band), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_UE_MODE, ue_mode, sizeof(ue_mode));
	if (err < 0) {
		snprintf(ue_mode, sizeof(ue_mode), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_OPERATOR, oper, sizeof(oper));
	if (err < 0) {
		snprintf(oper, sizeof(oper), "N/A");
	}
	err = modem_info_string_get(MODEM_INFO_TEMP, temp, sizeof(temp));
	if (err < 0) {
		snprintf(oper, sizeof(oper), "N/A");
	}

	snprintf(message, sizeof(message),
				"%s,,%lld,%s,%s,%s,%s,%.2f,%.2f,%d,%s,%s,%s,%s",
				imei,
				ping_rtt,
				rsrp,
				band,
				ue_mode,
				"90198",
				gnss_data->latitude,
				gnss_data->longitude,
				(int)gnss_data->accuracy,
				"99.99",temp,"999.99","99.99");
#else
	/* Format GNSS data as string */
	snprintf(message, sizeof(message),
		"GNSS: lat=%.2f, lon=%.2f, alt=%.2f, time=%04d-%02d-%02d %02d:%02d:%02d",
		(double)gnss_data->latitude, (double)gnss_data->longitude, (double)gnss_data->altitude,
		gnss_data->datetime.year, gnss_data->datetime.month, gnss_data->datetime.day,
		gnss_data->datetime.hour, gnss_data->datetime.minute, gnss_data->datetime.seconds);
#endif

	/* Send data */
	err = send(sock_fd, message, strlen(message), 0);
	if (err < 0) {
		LOG_ERR("Failed to send GNSS data, error: %d", errno);
		return -errno;
	}

	LOG_DBG("Sent GNSS data payload of %d bytes", strlen(message));
	return 0;
}


/* State handlers */

static void state_running_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	
	LOG_INF("Initializing NTN module");

	k_work_init(&timer_work, timer_work_handler);
	k_work_init(&gnss_location_work, gnss_location_work_handler);
	k_timer_init(&state->ntn_timer, ntn_timer_handler, NULL);

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);
		return;
	}

	/* Register GNSS event handler */
	nrf_modem_gnss_event_handler_set(gnss_event_handler);

	/* Register LTE event handler */
	lte_lc_register_handler(lte_lc_evt_handler);

	k_timer_start(&state->ntn_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);
}

static void state_running_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_TIMEOUT) {
			/* Timer expired, restart timer and transition to GNSS mode */
			k_timer_start(&state->ntn_timer, K_MINUTES(CONFIG_APP_NTN_TIMER_TIMEOUT_MINUTES), K_NO_WAIT);
			smf_set_state(SMF_CTX(state), &states[STATE_GNSS]);
		}
	}
}

static void state_gnss_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	/* Close socket if it was open */
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
		state->socket_connected = false;
	}

	LOG_INF("Entering GNSS mode");

	err = set_gnss_active_mode(state);
	if (err) {
		LOG_ERR("Unable to set GNSS mode");
		return;
	}

	err = nrf_modem_gnss_fix_interval_set(0);
	err = nrf_modem_gnss_fix_retry_set(180);
	err = nrf_modem_gnss_start();

}

static void state_gnss_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_LOCATION_SEARCH_DONE) {
			/* Location search completed, transition to NTN mode */
			smf_set_state(SMF_CTX(state), &states[STATE_NTN]);
		}
	}
}

static void state_gnss_exit(void *obj)
{
	int err;

	LOG_INF("Exiting GNSS mode");

	err = nrf_modem_gnss_stop();
	set_gnss_inactive_mode();
}

static void state_ntn_entry(void *obj)
{
	int err;
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	LOG_INF("Entering NTN mode");

	err = set_ntn_active_mode(state);
	if (err) {
		return;
	}

}

/* Helper function to calculate the standard ICMP checksum */
static uint16_t calculate_checksum(const void *buf, int len)
{
	const uint16_t *data = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += *data++;
		len -= 2;
	}
	if (len == 1) {
		uint16_t last_byte = 0;
		memcpy(&last_byte, data, 1);
		sum += last_byte;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return ~((uint16_t)sum);
}

static int ping_sock = -1;

int open_ping_socket(void)
{
	if (ping_sock >= 0) {
		LOG_DBG("Ping socket already open");
		return 0;
	}

	ping_sock = socket(AF_PACKET, SOCK_RAW, 0);
	if (ping_sock < 0) {
		LOG_ERR("Failed to create socket: %d", -errno);
		return -1;
	}

	LOG_DBG("Ping socket opened successfully: %d", ping_sock);
	return 0;
}

void close_ping_socket(void)
{
	if (ping_sock >= 0) {
		close(ping_sock);
		LOG_DBG("Ping socket closed: %d", ping_sock);
		ping_sock = -1;

	}
}

// Modify perform_ping to use the global socket
int64_t perform_ping(void)
{
	const char *target = "8.8.8.8";
	int ret;
	int64_t result = -1;
	struct sockaddr_in dest_addr = {
		.sin_family = AF_INET,
		.sin_port = 0  // Not used for ICMP
	};

	// Check if socket is open
	if (ping_sock < 0) {
		LOG_ERR("Ping socket not open");
		return -1;
	}

	// Declare these variables early since we'll use them for both flushing and receiving
	struct sockaddr_storage addr;
	socklen_t addr_len = sizeof(addr);

	// Flush the socket by reading any pending data
	uint8_t flush_buf[128];
	int flags = MSG_DONTWAIT;  // Non-blocking receive

	while (recvfrom(ping_sock, flush_buf, sizeof(flush_buf), flags,
					(struct sockaddr *)&addr, &addr_len) > 0) {
		// Continue reading until no more data is available
	}

	// Convert the target IP directly
	if (inet_pton(AF_INET, target, &dest_addr.sin_addr) != 1) {
		LOG_ERR("Failed to convert target IP address");
		return -1;
	}

	// Build the IPv4 + ICMP Echo Request packet
	#define IP_HDR_LEN 20
	#define ICMP_HDR_LEN 8
	#define TOTAL_LEN (IP_HDR_LEN + ICMP_HDR_LEN)

	uint8_t buf[TOTAL_LEN];
	memset(buf, 0, sizeof(buf));

	// IPv4 header
	buf[0] = (4 << 4) + (IP_HDR_LEN / 4);  // Version & header length
	buf[2] = TOTAL_LEN >> 8;                // Total length
	buf[3] = TOTAL_LEN & 0xFF;              // Total length
	buf[8] = 64;                            // TTL
	buf[9] = IPPROTO_ICMP;                  // Protocol

	// Get source IP address using modem_info
	char src_addr[NET_IPV4_ADDR_LEN] = {0};
	ret = modem_info_string_get(MODEM_INFO_IP_ADDRESS, src_addr, NET_IPV4_ADDR_LEN);
	if (ret < 0) {
		LOG_ERR("Failed to get source IP address: %d", ret);
		return -1;
	}

	// Convert IP string to bytes
	struct in_addr src_ip;
	if (inet_pton(AF_INET, src_addr, &src_ip) != 1) {
		LOG_ERR("Failed to convert source IP address");
		return -1;
	}
	memcpy(buf + 12, &src_ip.s_addr, 4);

	// Destination IP address (8.8.8.8)
	memcpy(buf + 16, &dest_addr.sin_addr.s_addr, 4);

	// Calculate IPv4 header checksum
	buf[10] = 0;
	buf[11] = 0;
	uint16_t ipv4_checksum = calculate_checksum(buf, IP_HDR_LEN);
	buf[10] = ipv4_checksum & 0xFF;
	buf[11] = ipv4_checksum >> 8;

	// ICMP header
	uint8_t *icmp = buf + IP_HDR_LEN;
	icmp[0] = 8;  // Echo Request
	icmp[4] = 0;  // ID high byte
	icmp[5] = 1;  // ID low byte
	icmp[6] = 0;  // Sequence high byte
	icmp[7] = 1;  // Sequence low byte

	// Calculate ICMP checksum
	uint16_t icmp_checksum = calculate_checksum(icmp, ICMP_HDR_LEN);
	icmp[2] = icmp_checksum & 0xFF;
	icmp[3] = icmp_checksum >> 8;

	int64_t start_time = k_uptime_get();
	LOG_INF("Sending ping to %s", target);
	ret = send(ping_sock, buf, sizeof(buf), 0);
	if (ret < 0) {
		LOG_ERR("send() failed: %d", -errno);
		return -1;
	}

	uint8_t recv_buf[64];
	LOG_INF("Receiving ping from %s", target);
	ret = recvfrom(ping_sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&addr, &addr_len);
	if (ret < 0) {
		LOG_ERR("recvfrom() failed or timed out: %d", -errno);
		return -1;
	}

	// Check if the received packet is an ICMP Echo Reply
	if (ret >= (IP_HDR_LEN + ICMP_HDR_LEN)) {
		uint8_t *icmp_reply = recv_buf + IP_HDR_LEN;
		if (icmp_reply[0] == 0) { // ICMP Echo Reply
			result = k_uptime_delta(&start_time);
			LOG_INF("Ping reply from %s: RTT = %lld ms", target, result);
		}
		else {
			LOG_ERR("Received packet is not an ICMP Echo Reply");
		}
	}
	else {
		LOG_ERR("Received packet is not an ICMP Echo Reply");
	}

	return result;
}

static void state_ntn_run(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;
	int err;

	if (state->chan == &NTN_CHAN) {
		struct ntn_msg *msg = (struct ntn_msg *)state->msg_buf;

		if (msg->type == NTN_NETWORK_CONNECTED) {
			LOG_DBG("Received NTN_NETWORK_CONNECTED, ping and setting up socket");

			if (open_ping_socket() != 0) {
				LOG_ERR("Failed to open ping socket");
				return;
			}

			int64_t ping_rtt = perform_ping();


			close_ping_socket();



			/* Network is connected, set up socket */
			err = sock_open_and_connect();
			if (err) {
				LOG_ERR("Failed to connect socket, error: %d", err);
				state->socket_connected = false;
			} else {
				LOG_DBG("Socket connected successfully");
				state->socket_connected = true;
				/* Send initial GNSS data if available */
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
					LOG_DBG("Sending initial GNSS data");
					err = sock_send_gnss_data(&last_pvt, ping_rtt);
					if (err) {
						LOG_ERR("Failed to send initial GNSS data, error: %d", err);
					} else {
						LOG_DBG("Initial GNSS data sent successfully");
					}
				} else {
					LOG_DBG("No valid GNSS data available to send initially");
				}
			}

		// For LEO, compute new wake up using SGP4 (Simplified General Perturbations Model 4)
		#if defined(CONFIG_APP_NTN_LEO)
			int sgp4_timeout_in_seconds;
			sgp4_timeout_in_seconds = sgp4_propagator_compute();
			with k_timer_start(&state->ntn_timer, K_SECONDS(sgp4_timeout_in_seconds), K_NO_WAIT);
		#endif

			/*
			In future, we should wait until we get ACK for data being transmitted,
			and cast CFUN=45 only after data were sent.
			It may take 10s to send data in NTN.
			k_sleep is added as intermediate solution
			*/
			k_sleep(K_MSEC(20000));

			err = set_ntn_dormant_mode();
			if (err) {
				return;
			}
		}
	}
}

static void state_ntn_exit(void *obj)
{
	struct ntn_state_object *state = (struct ntn_state_object *)obj;

	/* Close socket if it was open */
	if (sock_fd >= 0) {
		close(sock_fd);
		sock_fd = -1;
		state->socket_connected = false;
	}
}

static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	LOG_DBG("Network EVT TYPE received :%d",evt->type);
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_ERR("No SIM card detected!");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) {
			LOG_WRN("Not registered, check rejection cause");
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) {
			LOG_DBG("Network connectivity established to home network");
			ntn_msg_publish(NTN_NETWORK_CONNECTED);
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			LOG_DBG("Network connectivity established to roaming network");
			ntn_msg_publish(NTN_NETWORK_CONNECTED);
		}
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		if (evt->modem_evt == LTE_LC_MODEM_EVT_RESET_LOOP) {
			LOG_WRN("The modem has detected a reset loop!");
		} else if (evt->modem_evt == LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE) {
			LOG_DBG("LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE");
		}
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		if (evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED) {
			LOG_DBG("LTE_LC_RRC_MODE_CONNECTED");

		}
		else if (evt->rrc_mode == LTE_LC_RRC_MODE_IDLE) {
			LOG_DBG("LTE_LC_RRC_MODE_IDLE");
		}
		break;
	default:
		break;
	}
}

static void gnss_event_handler(int event)
{
	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT:
		/* Schedule work to handle PVT data in thread context */
		k_work_submit(&gnss_location_work);
		break;
	default:
		break;
	}
}

static void ntn_wdt_callback(int channel_id, void *user_data)
{
	LOG_ERR("NTN watchdog expired, Channel: %d, Thread: %s",
		channel_id, k_thread_name_get((k_tid_t)user_data));
}

static void ntn_module_thread(void)
{
	int err;
	int task_wdt_id;
	const uint32_t wdt_timeout_ms = CONFIG_APP_NTN_WATCHDOG_TIMEOUT_SECONDS * MSEC_PER_SEC;
	struct ntn_state_object ntn_state = { 0 };
	ntn_state.gnss_initialized=false;
	ntn_state.ntn_initialized=false;


	task_wdt_id = task_wdt_add(wdt_timeout_ms, ntn_wdt_callback, (void *)k_current_get());
	if (task_wdt_id < 0) {
		LOG_ERR("Failed to add task to watchdog: %d", task_wdt_id);
		return;
	}

	/* Initialize state machine */
	smf_set_initial(SMF_CTX(&ntn_state), &states[STATE_RUNNING]);

	while (true) {
		err = task_wdt_feed(task_wdt_id);
		if (err) {
			LOG_ERR("task_wdt_feed, error: %d", err);
			return;
		}

		/* Wait for messages */
		err = zbus_sub_wait_msg(&ntn, &ntn_state.chan, ntn_state.msg_buf, K_FOREVER);
		if (err) {
			LOG_ERR("Failed to receive message, error: %d", err);
			continue;
		}

		/* Run state machine */
		err = smf_run_state(SMF_CTX(&ntn_state));
		if (err) {
			LOG_ERR("Failed to run state machine, error: %d", err);
			continue;
		}
	}
}

K_THREAD_DEFINE(ntn_module_thread_id,
		CONFIG_APP_NTN_THREAD_STACK_SIZE,
		ntn_module_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);
