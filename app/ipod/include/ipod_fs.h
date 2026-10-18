// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_FS_H
#define IPOD_FS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief How long background tasks (decoder, art, scan) wait for a file slot. */
#define IPOD_FS_WAIT_WORKER_MS 3000u
/** @brief How long the UI thread waits (it must not stall the screen). */
#define IPOD_FS_WAIT_UI_MS 300u

/**
 * @brief fopen() that rides out a full SD card file table.
 *
 * TentacleOS mounts the card with a fixed number of file slots
 * (VFS_MAX_FILES, 5 by default) shared by the whole firmware. When they are
 * all taken, fopen() fails with ENFILE even though the file is fine. This
 * retries for up to @p wait_ms in that case only; any other error returns at
 * once. On failure errno is preserved, so ipod_fs_is_busy() can tell "card
 * busy" from "missing or unreadable".
 */
FILE *ipod_fopen(const char *path, const char *mode, uint32_t wait_ms);

/** @brief True when @p err means "no free file slot" (ENFILE / EMFILE). */
bool ipod_fs_is_busy(int err);

#ifdef __cplusplus
}
#endif

#endif // IPOD_FS_H
