/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <string.h>

#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#include "companion.h"
#include "companion_proto.h"

#if IS_ENABLED(CONFIG_COMPANION_CHUNKED_NTN)
#define SHOW_IMAGE_MAX (COMPANION_MAX_IMAGE * CONFIG_COMPANION_CHUNK_COUNT)
#else
#define SHOW_IMAGE_MAX COMPANION_MAX_IMAGE
#endif

/* Dark to bright luma ramp; one character covers 1 column x 2 rows of the thumbnail. */
static const char luma_ramp[] = " .:-=+*#%@";

static uint8_t wire[COMPANION_FRAME_MAX];
static uint8_t image[SHOW_IMAGE_MAX];

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint32_t frame_id;
	uint16_t score;
	size_t wire_len;
	int err;

	shell_print(sh, "pending=%s upload_busy=%s",
		    companion_has_pending() ? "yes" : "no",
		    companion_upload_busy() ? "yes" : "no");

	if (!companion_last_available()) {
		shell_print(sh, "last=none");
		return 0;
	}

	err = companion_last_info(&frame_id, &score, &wire_len);
	if (err) {
		shell_print(sh, "last=error %d", err);
		return 1;
	}

	shell_print(sh, "last frame=%u score_mille=%u wire_len=%u chunks=%u", frame_id, score,
		    (unsigned)wire_len, (unsigned)companion_last_wire_count());

	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	size_t wire_len;
	uint32_t frame_id;
	uint16_t score;
	int err;

	if (!companion_last_available()) {
		shell_print(sh, "No companion frame received yet");
		return 1;
	}

	err = companion_last_info(&frame_id, &score, &wire_len);
	if (err) {
		shell_print(sh, "last info failed: %d", err);
		return 1;
	}

	const size_t count = companion_last_wire_count();

	/* One COMPANION_WIRE line per chunk; PC tools split hex lines on it. */
	for (size_t idx = 0; idx < count; idx++) {
		err = companion_copy_last_wire(idx, wire, sizeof(wire), &wire_len);
		if (err) {
			shell_print(sh, "copy failed: %d", err);
			return 1;
		}

		shell_print(sh, "COMPANION_WIRE len=%u frame=%u score_mille=%u chunk=%u/%u",
			    (unsigned)wire_len, frame_id, score, (unsigned)idx + 1U,
			    (unsigned)count);
		for (size_t off = 0; off < wire_len; off += 64U) {
			const size_t chunk = MIN(wire_len - off, 64U);

			shell_fprintf(sh, SHELL_NORMAL, "COMPANION_HEX");
			for (size_t i = 0; i < chunk; i++) {
				shell_fprintf(sh, SHELL_NORMAL, "%02x", wire[off + i]);
			}
			shell_fprintf(sh, SHELL_NORMAL, "\n");
		}
	}
	shell_print(sh, "COMPANION_END");

	return 0;
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char line[128 + 1];
	size_t image_len;
	size_t wire_len;
	uint32_t frame_id;
	uint16_t score;
	uint16_t w;
	uint16_t h;
	int err;

	if (!companion_last_available()) {
		shell_print(sh, "No companion frame received yet");
		return 1;
	}

	err = companion_last_info(&frame_id, &score, &wire_len);
	if (!err) {
		err = companion_copy_last_image(image, sizeof(image), &image_len, &w, &h);
	}
	if (err) {
		shell_print(sh, "copy failed: %d", err);
		return 1;
	}

	if (w == 0 || w >= sizeof(line) || (size_t)w * h != image_len) {
		shell_print(sh, "Unexpected thumbnail %ux%u for %u bytes", w, h,
			    (unsigned)image_len);
		return 1;
	}

	uint8_t lo = UINT8_MAX;
	uint8_t hi = 0;

	for (size_t i = 0; i < image_len; i++) {
		lo = MIN(lo, image[i]);
		hi = MAX(hi, image[i]);
	}

	const uint32_t span = MAX(hi - lo, 1);

	shell_print(sh, "frame %u, score %u.%03u, %ux%u luma (contrast stretched %u..%u)",
		    frame_id, score / 1000U, score % 1000U, w, h, lo, hi);

	for (uint16_t y = 0; y < h; y += 2) {
		for (uint16_t x = 0; x < w; x++) {
			uint32_t luma = image[y * w + x];

			if (y + 1U < h) {
				luma = (luma + image[(y + 1U) * w + x] + 1U) / 2U;
			}
			luma = ((luma - lo) * (sizeof(luma_ramp) - 2U) + span / 2U) / span;
			line[x] = luma_ramp[luma];
		}
		line[w] = '\0';
		shell_print(sh, "%s", line);
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_att_companion,
	SHELL_CMD(status, NULL, "Show pending / last received frame", cmd_status),
	SHELL_CMD(dump, NULL, "Print last frame (all chunks) as hex for PC tools", cmd_dump),
	SHELL_CMD(show, NULL, "Draw last received thumbnail as ASCII art", cmd_show),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_companion, &sub_att_companion, "Companion UART debug", NULL);
