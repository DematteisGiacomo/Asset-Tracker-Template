/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "companion.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "companion_proto.h"
#include "ntn.h"

LOG_MODULE_REGISTER(companion_module, CONFIG_APP_COMPANION_LOG_LEVEL);

#if IS_ENABLED(CONFIG_APP_COMPANION)

static const struct device *const uart_dev = DEVICE_DT_GET(DT_ALIAS(companion_uart));

static K_MUTEX_DEFINE(pending_lock);
static uint8_t pending_wire[COMPANION_FRAME_MAX];
static size_t pending_wire_len;
static bool pending_valid;

enum rx_state {
	RX_HEADER,
	RX_IMAGE,
	RX_CRC,
};

static struct {
	enum rx_state state;
	uint8_t buf[COMPANION_FRAME_MAX];
	size_t len;
	uint16_t image_len;
} rx;

static atomic_t rx_byte_total;
static atomic_t rx_frame_ok;

static void send_uart_ack(void)
{
	uart_poll_out(uart_dev, COMPANION_UART_ACK);
}

static int store_frame_and_trigger(const uint8_t *wire, size_t wire_len)
{
	struct companion_detect_image decoded;
	struct ntn_msg msg = { .type = NTN_TRIGGER };
	int err;

	err = companion_frame_decode(wire, wire_len, &decoded);
	if (err) {
		LOG_INF("Companion frame decode failed: %d len=%u hdr=%02x %02x %02x %02x", err,
			(unsigned)wire_len, wire[0], wire[1], wire[2], wire[3]);
		return err;
	}

	k_mutex_lock(&pending_lock, K_FOREVER);
	memcpy(pending_wire, wire, wire_len);
	pending_wire_len = wire_len;
	pending_valid = true;
	k_mutex_unlock(&pending_lock);

	atomic_inc(&rx_frame_ok);
	LOG_INF("Companion image pending: frame %u score %u len %u",
		decoded.hdr.frame_id, decoded.hdr.top_score_mille, decoded.hdr.image_len);

	send_uart_ack();

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN_TRIGGER: %d", err);
	}

	return 0;
}

static void rx_reset(void)
{
	rx.state = RX_HEADER;
	rx.len = 0;
	rx.image_len = 0;
}

static void rx_shift_one(void)
{
	if (rx.len <= 1U) {
		rx_reset();
		return;
	}

	memmove(rx.buf, rx.buf + 1, rx.len - 1);
	rx.len--;
	rx.state = RX_HEADER;
	rx.image_len = 0;
}

static void rx_feed_byte(uint8_t byte)
{
	switch (rx.state) {
	case RX_HEADER:
		rx.buf[rx.len++] = byte;
		while (rx.len >= COMPANION_HEADER_SIZE) {
			uint16_t ilen;
			int hres = companion_header_from_wire(rx.buf, rx.len, &ilen);

			if (hres == 0) {
				rx.image_len = ilen;
				rx.state = RX_IMAGE;
				break;
			}
			rx_shift_one();
		}
		break;
	case RX_IMAGE:
		rx.buf[rx.len++] = byte;
		if (rx.len >= COMPANION_HEADER_SIZE + rx.image_len) {
			rx.state = RX_CRC;
		}
		break;
	case RX_CRC:
		rx.buf[rx.len++] = byte;
		if (rx.len >= companion_wire_payload_len(rx.image_len)) {
			if (store_frame_and_trigger(rx.buf, rx.len) == 0) {
				rx_reset();
			} else {
				rx_shift_one();
			}
		}
		break;
	default:
		rx_reset();
		break;
	}

	if (rx.len >= sizeof(rx.buf)) {
		rx_reset();
	}
}

static void companion_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (!device_is_ready(uart_dev)) {
		LOG_WRN("Companion UART not ready, retrying...");
		k_msleep(500);
	}

	LOG_INF("Companion UART RX active");
	rx_reset();

	while (true) {
		uint8_t byte;
		int ret = uart_poll_in(uart_dev, &byte);

		if (ret == -1) {
			k_msleep(10);
			continue;
		}

		atomic_inc(&rx_byte_total);
		if ((atomic_get(&rx_byte_total) % 256U) == 1U) {
			LOG_INF("Companion RX bytes=%ld frames_ok=%ld",
				(long)atomic_get(&rx_byte_total),
				(long)atomic_get(&rx_frame_ok));
		}

		rx_feed_byte(byte);
	}
}

K_THREAD_DEFINE(companion_thread, CONFIG_APP_COMPANION_THREAD_STACK_SIZE, companion_thread_fn,
		NULL, NULL, NULL, K_PRIO_PREEMPT(CONFIG_APP_COMPANION_THREAD_PRIORITY), 0, 0);

bool companion_has_pending(void)
{
	bool val;

	k_mutex_lock(&pending_lock, K_FOREVER);
	val = pending_valid;
	k_mutex_unlock(&pending_lock);

	return val;
}

int companion_copy_wire(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	if (buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&pending_lock, K_FOREVER);
	if (!pending_valid || pending_wire_len > buf_cap) {
		k_mutex_unlock(&pending_lock);
		return pending_valid ? -ENOSPC : -ENOENT;
	}

	memcpy(buf, pending_wire, pending_wire_len);
	*out_len = pending_wire_len;
	k_mutex_unlock(&pending_lock);

	return 0;
}

void companion_clear_pending(void)
{
	k_mutex_lock(&pending_lock, K_FOREVER);
	pending_valid = false;
	pending_wire_len = 0;
	k_mutex_unlock(&pending_lock);
}

/*
 * Do not fail SYS_INIT if UART is not ready yet — a negative return aborts boot.
 * The companion thread waits for device_is_ready() instead.
 */
static int companion_init(void)
{
	return 0;
}

SYS_INIT(companion_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else

bool companion_has_pending(void)
{
	return false;
}

int companion_copy_wire(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(buf_cap);
	ARG_UNUSED(out_len);

	return -ENOTSUP;
}

void companion_clear_pending(void)
{
}

#endif
