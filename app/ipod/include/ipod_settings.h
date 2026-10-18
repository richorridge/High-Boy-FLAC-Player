// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_SETTINGS_H
#define IPOD_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#include "ipod_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_VOLUME_MAX     100
#define IPOD_VOLUME_DEFAULT 60

typedef struct {
  uint8_t volume;       ///< 0..IPOD_VOLUME_MAX
  uint8_t volume_limit; ///< IPOD_VOLUME_MAX = no limit
  ipod_shuffle_t shuffle;
  ipod_repeat_t repeat;
  int eq_preset; ///< ipod_eq_name() index, 0 = Off
  bool sound_check;
  bool clicker;
} ipod_settings_t;

void ipod_settings_defaults(ipod_settings_t *s);
/** @brief Load from a key=value file; missing keys keep their defaults. */
bool ipod_settings_load(ipod_settings_t *s, const char *path);
bool ipod_settings_save(const ipod_settings_t *s, const char *path);

#ifdef __cplusplus
}
#endif

#endif // IPOD_SETTINGS_H
