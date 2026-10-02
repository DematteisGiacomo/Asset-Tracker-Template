/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef COMPANION_H__
#define COMPANION_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool companion_has_pending(void);

/** Copy next encoded wire frame for UDP uplink (chunk index for multi-packet mode). */
int companion_copy_wire(uint8_t *buf, size_t buf_cap, size_t *out_len);

void companion_clear_pending(void);

/** Drop upload-in-progress gate without clearing pending (failed/aborted NTN cycle). */
void companion_clear_upload_busy(void);

bool companion_upload_busy(void);

/**
 * Call after NTN_SEND_ACK when a companion upload was in progress.
 * Returns true if another chunk must be sent on the same NTN session.
 */
bool companion_ntn_advance_after_ack(void);

/** Current NTN chunk index (0-based) and total when a multi-chunk upload is pending. */
int companion_ntn_send_progress(uint8_t *index_out, uint8_t *total_out);

/** Last successfully received frame, all chunks (kept after NTN clears pending). */
bool companion_last_available(void);

/** wire_len is the total over all wire frames (chunks) of the last frame. */
int companion_last_info(uint32_t *frame_id, uint16_t *score_mille, size_t *wire_len);

/** Number of wire frames (chunks) in the last frame, 0 if none. */
size_t companion_last_wire_count(void);

int companion_copy_last_wire(size_t idx, uint8_t *buf, size_t buf_cap, size_t *out_len);

/** Reassembled thumbnail payload of the last frame. */
int companion_copy_last_image(uint8_t *buf, size_t buf_cap, size_t *out_len, uint16_t *width,
			      uint16_t *height);

#endif /* COMPANION_H__ */
