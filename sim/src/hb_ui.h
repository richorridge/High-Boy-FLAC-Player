// Copyright (c) 2026 High Boy iPod app contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HB_UI_H
#define HB_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "input_manager.h"

void hb_input_set(input_button_t b, bool down, uint32_t now_ms);
void hb_input_tick(uint32_t now_ms);
void hb_ui_pump(void);
bool hb_ui_quit_requested(void);

#endif // HB_UI_H
