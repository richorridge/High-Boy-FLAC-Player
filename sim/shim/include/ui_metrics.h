// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: the subset of the TentacleOS header of the same
// name used by the app, with identical declarations.

#ifndef UI_METRICS_H
#define UI_METRICS_H

#include "lvgl.h"

static inline int32_t ui_screen_w(void) {
  return lv_display_get_horizontal_resolution(NULL);
}

static inline int32_t ui_screen_h(void) {
  return lv_display_get_vertical_resolution(NULL);
}

#endif // UI_METRICS_H
