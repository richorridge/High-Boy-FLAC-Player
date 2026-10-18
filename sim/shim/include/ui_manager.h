// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: the subset of the TentacleOS header of the same
// name used by the app, with identical declarations.

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <stdbool.h>

#include "lvgl.h"
#include "input_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// The real enum has ~170 entries; the simulator only knows the two screens it
// hosts. SCREEN_GAMES_MENU is the TentacleOS "APPS" menu the iPod returns to.
typedef enum { SCREEN_NONE, SCREEN_GAMES_MENU, SCREEN_IPOD, SCREEN_COUNT } screen_id_t;

bool ui_acquire(void);
void ui_release(void);
void ui_async_call(lv_async_cb_t cb, void *user_data);
void ui_switch_screen(screen_id_t new_screen);
void ui_screen_load_owned(lv_obj_t **slot, lv_obj_t *scr);
screen_id_t ui_current_screen(void);

typedef void (*ui_input_handler_t)(const input_event_t *ev, void *ctx);
void ui_input_set_screen_handler(ui_input_handler_t handler, void *ctx);
bool ui_input_is_locked(void);

#ifdef __cplusplus
}
#endif

#endif // UI_MANAGER_H
