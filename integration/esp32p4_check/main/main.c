// Copyright (c) 2026 High Boy iPod app contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Entry point of the ESP32-P4 build check: references the app's public entry
// points so the linker pulls in (and resolves) all of it.

#include "lvgl.h"
#include "ui_ipod.h"

void app_main(void) {
  lv_init();
  ui_ipod_set_return(0);
  ui_ipod_open();
  ui_ipod_debug_t d;
  ui_ipod_debug(&d);
  ui_ipod_stop();
}
