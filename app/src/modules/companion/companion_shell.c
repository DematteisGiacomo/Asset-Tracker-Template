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

	shell_print(sh, "last frame=%u score_mille=%u wire_len=%u", frame_id, score,
		    (unsigned)wire_len);

	return 0;
}

static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t wire[COMPANION_FRAME_MAX];
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

	err = companion_copy_last_wire(wire, sizeof(wire), &wire_len);
	if (err) {
		shell_print(sh, "copy failed: %d", err);
		return 1;
	}

	shell_print(sh, "COMPANION_WIRE len=%u frame=%u score_mille=%u", (unsigned)wire_len,
		    frame_id, score);
	for (size_t off = 0; off < wire_len; off += 64U) {
		const size_t chunk = MIN(wire_len - off, 64U);

		shell_fprintf(sh, SHELL_NORMAL, "COMPANION_HEX");
		for (size_t i = 0; i < chunk; i++) {
			shell_fprintf(sh, SHELL_NORMAL, "%02x", wire[off + i]);
		}
		shell_fprintf(sh, SHELL_NORMAL, "\n");
	}
	shell_print(sh, "COMPANION_END");

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_att_companion,
	SHELL_CMD(status, NULL, "Show pending / last received frame", cmd_status),
	SHELL_CMD(dump, NULL, "Print last wire frame as hex for PC tools", cmd_dump),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(att_companion, &sub_att_companion, "Companion UART debug", NULL);
