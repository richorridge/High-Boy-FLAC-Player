// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// Simulator-only hooks (never compiled into firmware).

#ifndef HB_SIM_H
#define HB_SIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint64_t samples_out;
  uint32_t stream_starts;
  uint32_t stream_stops;
  uint32_t ui_clicks;
  uint32_t last_rate;
  bool streaming;
  uint8_t volume_pct;
} hb_audio_stats_t;

void hb_audio_get_stats(hb_audio_stats_t *out);

/** @brief Map the device's /sdcard mount point onto a host folder. */
void hb_fs_set_sd_root(const char *host_dir);

/** @brief Card files currently open (limited to HB_MAX_FILES, default 5). */
int hb_fs_open_count(void);

#ifdef __cplusplus
}
#endif

#endif // HB_SIM_H
