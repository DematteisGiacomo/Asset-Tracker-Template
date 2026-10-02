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
#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
static uint8_t ntn_wires[CONFIG_COMPANION_CHUNK_COUNT][COMPANION_FRAME_MAX];
static size_t ntn_wire_lens[CONFIG_COMPANION_CHUNK_COUNT];
static uint8_t ntn_send_idx;
static uint8_t ntn_chunk_total;
static struct {
	bool active;
	uint32_t frame_id;
	uint32_t chunk_mask;
} reasm;
#endif

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
#define LAST_WIRE_SLOTS CONFIG_COMPANION_CHUNK_COUNT
#else
#define LAST_WIRE_SLOTS 1
#endif

static K_MUTEX_DEFINE(last_lock);
static uint8_t last_wires[LAST_WIRE_SLOTS][COMPANION_FRAME_MAX];
static size_t last_wire_lens[LAST_WIRE_SLOTS];
static uint8_t last_wire_count;
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

#define UART_RX_RING_SIZE 8192U
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

static void reasm_reset(void)
{
#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
	reasm.active = false;
	reasm.frame_id = 0;
	reasm.chunk_mask = 0;
#endif
}

static int publish_ntn_trigger(void)
{
	struct ntn_msg msg = { .type = NTN_TRIGGER };
	int err;

	err = zbus_chan_pub(&NTN_CHAN, &msg, K_SECONDS(1));
	if (err) {
		LOG_ERR("Failed to publish NTN_TRIGGER: %d", err);
		companion_clear_pending();
	}

	return err;
}

static int store_single_frame(const uint8_t *wire, size_t wire_len)
{
	if (IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD)) {
		k_mutex_lock(&pending_lock, K_FOREVER);
		memcpy(pending_wire, wire, wire_len);
		pending_wire_len = wire_len;
		pending_valid = true;
#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
		ntn_chunk_total = 1;
		ntn_send_idx = 0;
#endif
		k_mutex_unlock(&pending_lock);
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	memcpy(last_wires[0], wire, wire_len);
	last_wire_lens[0] = wire_len;
	last_wire_count = 1;
	last_valid = true;
	last_frame_id = decode_scratch.hdr.frame_id;
	last_score_mille = decode_scratch.hdr.top_score_mille;
	k_mutex_unlock(&last_lock);

	atomic_inc(&rx_frame_ok);

	if (!IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD)) {
		LOG_INF("Companion image received: frame %u score %u len %u (NTN upload disabled)",
			decode_scratch.hdr.frame_id, decode_scratch.hdr.top_score_mille,
			decode_scratch.hdr.image_len);
		send_uart_ack();
		return 0;
	}

	atomic_set(&upload_busy, 1);
	LOG_INF("Companion image pending: frame %u score %u len %u",
		decode_scratch.hdr.frame_id, decode_scratch.hdr.top_score_mille,
		decode_scratch.hdr.image_len);

	send_uart_ack();

	return publish_ntn_trigger();
}

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
static int store_chunk_frame(const uint8_t *wire, size_t wire_len)
{
	const uint8_t idx = decode_scratch.hdr.detect_count;
	const uint8_t total = decode_scratch.hdr.image_fmt;

	if (total != CONFIG_COMPANION_CHUNK_COUNT || idx >= total) {
		LOG_WRN("Invalid chunk meta idx=%u total=%u", idx, total);
		return -EINVAL;
	}

	if (!reasm.active || reasm.frame_id != decode_scratch.hdr.frame_id) {
		reasm.active = true;
		reasm.frame_id = decode_scratch.hdr.frame_id;
		reasm.chunk_mask = 0;
		if (IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD)) {
			atomic_set(&upload_busy, 1);
		}
	}

	if ((reasm.chunk_mask & BIT(idx)) != 0U) {
		LOG_WRN("Duplicate chunk %u for frame %u", idx, reasm.frame_id);
		return -EALREADY;
	}

	memcpy(ntn_wires[idx], wire, wire_len);
	ntn_wire_lens[idx] = wire_len;
	reasm.chunk_mask |= BIT(idx);

	atomic_inc(&rx_frame_ok);
	LOG_INF("Companion chunk %u/%u frame %u (%u B)", idx + 1U, total, reasm.frame_id,
		decode_scratch.hdr.image_len);

	if (reasm.chunk_mask != ((1U << total) - 1U)) {
		return 0;
	}

	if (IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD)) {
		k_mutex_lock(&pending_lock, K_FOREVER);
		pending_valid = true;
		ntn_chunk_total = total;
		ntn_send_idx = 0;
		k_mutex_unlock(&pending_lock);
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	for (uint8_t i = 0; i < total; i++) {
		memcpy(last_wires[i], ntn_wires[i], ntn_wire_lens[i]);
		last_wire_lens[i] = ntn_wire_lens[i];
	}
	last_wire_count = total;
	last_valid = true;
	last_frame_id = decode_scratch.hdr.frame_id;
	last_score_mille = decode_scratch.hdr.top_score_mille;
	k_mutex_unlock(&last_lock);

	LOG_INF("Companion reassembly complete: frame %u (%u chunks, score %u)%s", reasm.frame_id,
		total, decode_scratch.hdr.top_score_mille,
		IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD) ? "" : " (NTN upload disabled)");
	send_uart_ack();
	reasm_reset();

	if (!IS_ENABLED(CONFIG_APP_COMPANION_NTN_UPLOAD)) {
		return 0;
	}

	return publish_ntn_trigger();
}
#endif

static int store_frame_and_trigger(const uint8_t *wire, size_t wire_len)
{
	int err;
	bool allow_while_busy = false;

	err = companion_frame_decode(wire, wire_len, &decode_scratch);
	if (err) {
		LOG_WRN("Companion frame decode failed: %d len=%u (CRC=-EBADMSG=%d)", err,
			(unsigned)wire_len, -EBADMSG);
		return err;
	}

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
	if (decode_scratch.hdr.msg_type == COMPANION_MSG_DETECT_IMAGE_CHUNK && reasm.active &&
	    decode_scratch.hdr.frame_id == reasm.frame_id) {
		allow_while_busy = true;
	}
#endif

	if (atomic_get(&upload_busy) != 0 && !allow_while_busy) {
		atomic_inc(&rx_frame_discarded);
		LOG_WRN("Companion frame discarded: upload in progress (drops=%ld)",
			(long)atomic_get(&rx_frame_discarded));
		return 0;
	}

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
	if (decode_scratch.hdr.msg_type == COMPANION_MSG_DETECT_IMAGE_CHUNK) {
		return store_chunk_frame(wire, wire_len);
	}
#endif

	return store_single_frame(wire, wire_len);
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
			int hres = companion_header_from_wire(rx.buf, rx.len, &ilen, NULL);

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
	if (!pending_valid) {
		k_mutex_unlock(&pending_lock);
		return -ENOENT;
	}

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
	if (ntn_chunk_total > 1U) {
		const size_t len = ntn_wire_lens[ntn_send_idx];

		if (len > buf_cap) {
			k_mutex_unlock(&pending_lock);
			return -ENOSPC;
		}
		memcpy(buf, ntn_wires[ntn_send_idx], len);
		*out_len = len;
		k_mutex_unlock(&pending_lock);
		return 0;
	}
#endif

	if (pending_wire_len > buf_cap) {
		k_mutex_unlock(&pending_lock);
		return -ENOSPC;
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
#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
	ntn_send_idx = 0;
	ntn_chunk_total = 0;
#endif
	k_mutex_unlock(&pending_lock);

	reasm_reset();
	atomic_set(&upload_busy, 0);
}

bool companion_ntn_advance_after_ack(void)
{
	bool more;

	k_mutex_lock(&pending_lock, K_FOREVER);
	if (!pending_valid || ntn_chunk_total <= 1U) {
		k_mutex_unlock(&pending_lock);
		return false;
	}

	ntn_send_idx++;
	more = ntn_send_idx < ntn_chunk_total;
	if (!more) {
		pending_valid = false;
		ntn_send_idx = 0;
		ntn_chunk_total = 0;
	}
	k_mutex_unlock(&pending_lock);

	if (!more) {
		atomic_set(&upload_busy, 0);
	}

	return more;
}

int companion_ntn_send_progress(uint8_t *index_out, uint8_t *total_out)
{
	if (index_out == NULL || total_out == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&pending_lock, K_FOREVER);
	if (!pending_valid || ntn_chunk_total <= 1U) {
		k_mutex_unlock(&pending_lock);
		return -ENOENT;
	}

	*index_out = ntn_send_idx;
	*total_out = ntn_chunk_total;
	k_mutex_unlock(&pending_lock);

	return 0;
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
	*wire_len = 0;
	for (uint8_t i = 0; i < last_wire_count; i++) {
		*wire_len += last_wire_lens[i];
	}
	k_mutex_unlock(&last_lock);

	return 0;
}

size_t companion_last_wire_count(void)
{
	size_t count;

	k_mutex_lock(&last_lock, K_FOREVER);
	count = last_valid ? last_wire_count : 0;
	k_mutex_unlock(&last_lock);

	return count;
}

int companion_copy_last_wire(size_t idx, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	if (buf == NULL || out_len == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	if (!last_valid || idx >= last_wire_count) {
		k_mutex_unlock(&last_lock);
		return -ENOENT;
	}
	if (last_wire_lens[idx] > buf_cap) {
		k_mutex_unlock(&last_lock);
		return -ENOSPC;
	}

	memcpy(buf, last_wires[idx], last_wire_lens[idx]);
	*out_len = last_wire_lens[idx];
	k_mutex_unlock(&last_lock);

	return 0;
}

int companion_copy_last_image(uint8_t *buf, size_t buf_cap, size_t *out_len, uint16_t *width,
			      uint16_t *height)
{
	struct companion_header hdr = {0};
	size_t total = 0;

	if (buf == NULL || out_len == NULL || width == NULL || height == NULL) {
		return -EINVAL;
	}

	k_mutex_lock(&last_lock, K_FOREVER);
	if (!last_valid) {
		k_mutex_unlock(&last_lock);
		return -ENOENT;
	}

	/* Chunks are stored at their chunk index, so concatenation restores row order. */
	for (uint8_t i = 0; i < last_wire_count; i++) {
		memcpy(&hdr, last_wires[i], sizeof(hdr));

		if (total + hdr.image_len > buf_cap) {
			k_mutex_unlock(&last_lock);
			return -ENOSPC;
		}

		memcpy(buf + total, last_wires[i] + COMPANION_HEADER_SIZE, hdr.image_len);
		total += hdr.image_len;
	}
	k_mutex_unlock(&last_lock);

	*out_len = total;
	*width = hdr.thumb_w;
	*height = hdr.thumb_h;

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

bool companion_ntn_advance_after_ack(void)
{
	return false;
}

int companion_ntn_send_progress(uint8_t *index_out, uint8_t *total_out)
{
	ARG_UNUSED(index_out);
	ARG_UNUSED(total_out);

	return -ENOTSUP;
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

size_t companion_last_wire_count(void)
{
	return 0;
}

int companion_copy_last_wire(size_t idx, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
	ARG_UNUSED(idx);
	ARG_UNUSED(buf);
	ARG_UNUSED(buf_cap);
	ARG_UNUSED(out_len);

	return -ENOTSUP;
}

int companion_copy_last_image(uint8_t *buf, size_t buf_cap, size_t *out_len, uint16_t *width,
			      uint16_t *height)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(buf_cap);
	ARG_UNUSED(out_len);
	ARG_UNUSED(width);
	ARG_UNUSED(height);

	return -ENOTSUP;
}

#endif
