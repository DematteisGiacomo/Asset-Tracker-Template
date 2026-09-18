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

/** Copy encoded wire frame for UDP uplink. Returns 0 or negative errno. */
int companion_copy_wire(uint8_t *buf, size_t buf_cap, size_t *out_len);

void companion_clear_pending(void);

/** Drop upload-in-progress gate without clearing pending (failed/aborted NTN cycle). */
void companion_clear_upload_busy(void);

bool companion_upload_busy(void);

/** Last successfully received wire frame (kept after NTN clears pending). */
bool companion_last_available(void);

int companion_last_info(uint32_t *frame_id, uint16_t *score_mille, size_t *wire_len);

int companion_copy_last_wire(uint8_t *buf, size_t buf_cap, size_t *out_len);

#endif /* COMPANION_H__ */
