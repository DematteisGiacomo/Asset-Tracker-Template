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

static K_MUTEX_DEFINE(last_lock);
static uint8_t last_wire[COMPANION_FRAME_MAX];
static size_t last_wire_len;
static bool last_valid;
static uint32_t last_frame_id;
static uint16_t last_score_mille;

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
static atomic_t rx_frame_discarded;
static atomic_t rx_ring_drop;
static atomic_t upload_busy;

#define UART_RX_RING_SIZE 4096U
static uint8_t uart_rx_ring[UART_RX_RING_SIZE];
static volatile uint16_t uart_rx_head;
static volatile uint16_t uart_rx_tail;

static void uart_rx_ring_put(uint8_t byte)
{
	uint16_t next = (uint16_t)((uart_rx_head + 1U) % UART_RX_RING_SIZE);

	if (next == uart_rx_tail) {
		atomic_inc(&rx_ring_drop);
		return;
	}

	uart_rx_ring[uart_rx_head] = byte;
	uart_rx_head = next;
}

static int uart_rx_ring_get(uint8_t *byte)
{
	if (uart_rx_head == uart_rx_tail) {
		return -1;
	}

	*byte = uart_rx_ring[uart_rx_tail];
	uart_rx_tail = (uint16_t)((uart_rx_tail + 1U) % UART_RX_RING_SIZE);

	return 0;
}

static void companion_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (!uart_irq_rx_ready(dev)) {
			continue;
		}

		uint8_t chunk[16];
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));

		for (int i = 0; i < n; i++) {
			uart_rx_ring_put(chunk[i]);
		}
	}
}

static void send_uart_ack(void)
{
	uart_poll_out(uart_dev, COMPANION_UART_ACK);
}

static struct companion_detect_image decode_scratch;

static int store_frame_and_trigger(const uint8_t *wire, size_t wire_len)
{
	struct ntn_msg msg = { .type = NTN_TRIGGER };
	int err;

	if (atomic_get(&upload_busy) != 0) {
		atomic_inc(&rx_frame_discarded);
		LOG_WRN("Companion frame discarded: upload in progress (drops=%ld)",
			(long)atomic_get(&rx_frame_discarded));
		return 0;
	}

	err = companion_frame_decode(wire, wire_len, &decode_scratch);
	if (err) {
		LOG_WRN("Companion frame decode failed: %d len=%u (CRC=-EBADMSG=%d)", err,
			(unsigned)wire_len, -EBADMSG);
		return err;
	}

	k_mutex_lock(&pending_lock, K_FOREVER);
	memcpy(pending_wire, wire, wire_len);
	pending_wire_len = wire_len;
	pending_valid = true;
	k_mutex_unlock(&pending_lock);

	k_mutex_lock(&last_lock, K_FOREVER);
	memcpy(last_wire, wire, wire_len);
	last_wire_len = wire_len;
	last_valid = true;
	last_frame_id = decode_scratch.hdr.frame_id;
	last_score_mille = decode_scratch.hdr.top_score_mille;
	k_mutex_unlock(&last_lock);

	atomic_set(&upload_busy, 1);
	atomic_inc(&rx_frame_ok);
	LOG_INF("Companion image pending: frame %u score %u len %u",
		decode_scratch.hdr.frame_id, decode_scratch.hdr.top_score_mille,
		decode_scratch.hdr.image_len);

	send_uart_ack();

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN_TRIGGER: %d", err);
		companion_clear_pending();
		return err;
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

	uart_irq_callback_user_data_set(uart_dev, companion_uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	LOG_INF("Companion UART RX active (IRQ + ring)");
	rx_reset();

	while (true) {
		uint8_t byte;
		int ret = uart_rx_ring_get(&byte);

		if (ret == -1) {
			if (atomic_get(&rx_ring_drop) != 0) {
				LOG_WRN("Companion UART RX ring overflow (drops=%ld)",
					(long)atomic_get(&rx_ring_drop));
			}
			k_msleep(1);
			continue;
		}

		do {
			atomic_inc(&rx_byte_total);
			if (atomic_get(&rx_byte_total) <= 4U) {
				LOG_DBG("Companion RX byte %ld: 0x%02x",
					(long)atomic_get(&rx_byte_total), byte);
			}

			rx_feed_byte(byte);
			ret = uart_rx_ring_get(&byte);
		} while (ret == 0);
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

	atomic_set(&upload_busy, 0);
}

void companion_clear_upload_busy(void)
{
	atomic_set(&upload_busy, 0);
}

bool companion_upload_busy(void)
{
	return atomic_get(&upload_busy) != 0;
}

bool companion_last_available(void)
{
	bool val;

	k_mutex_lock(&last_lock, K_FOREVER);
	val = last_valid;
	k_mutex_unlock(&last_lock);

	return val;
}

int companion_last_info(uint32_t *frame_id, uint16_t *score_mille, size_t *wire_len)
{
	if (frame_id == NULL || score_mille == NULL || wire_len == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	if (!last_valid) {
		k_mutex_unlock(&last_lock);
		return -ENOENT;
	}

	*frame_id = last_frame_id;
	*score_mille = last_score_mille;
	*wire_len = last_wire_len;
	k_mutex_unlock(&last_lock);

	return 0;
}

int companion_copy_last_wire(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	if (buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	if (!last_valid || last_wire_len > buf_cap) {
		k_mutex_unlock(&last_lock);
		return last_valid ? -ENOSPC : -ENOENT;
	}

	memcpy(buf, last_wire, last_wire_len);
	*out_len = last_wire_len;
	k_mutex_unlock(&last_lock);

	return 0;
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

void companion_clear_upload_busy(void)
{
}

bool companion_upload_busy(void)
{
	return false;
}

bool companion_last_available(void)
{
	return false;
}

int companion_last_info(uint32_t *frame_id, uint16_t *score_mille, size_t *wire_len)
{
	ARG_UNUSED(frame_id);
	ARG_UNUSED(score_mille);
	ARG_UNUSED(wire_len);

	return -ENOTSUP;
}

int companion_copy_last_wire(uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(buf_cap);
	ARG_UNUSED(out_len);

	return -ENOTSUP;
}

#endif
