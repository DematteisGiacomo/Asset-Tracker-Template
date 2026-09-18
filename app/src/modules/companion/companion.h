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

#endif /* COMPANION_H__ */
