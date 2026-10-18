// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: the subset of the TentacleOS header of the same
// name used by the app, with identical declarations.

#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  INPUT_BTN_UP = 0,
  INPUT_BTN_DOWN,
  INPUT_BTN_LEFT,
  INPUT_BTN_RIGHT,
  INPUT_BTN_OK,
  INPUT_BTN_BACK,
  INPUT_BTN_COUNT,
} input_button_t;

typedef enum {
  INPUT_ACTION_PRESS = 0,  ///< debounced edge: button went down
  INPUT_ACTION_RELEASE,    ///< debounced edge: button came up
  INPUT_ACTION_LONG_PRESS, ///< held past the long-press threshold (fires once)
  INPUT_ACTION_REPEAT,     ///< auto-repeat while held (after the initial delay)
} input_action_t;

typedef struct {
  input_button_t button;
  input_action_t action;
  uint32_t timestamp_ms; ///< ms since boot when the event was produced
} input_event_t;

esp_err_t input_manager_init(void);
bool input_get_event(input_event_t *out_event, uint32_t timeout_ms);
bool input_is_down(input_button_t button);
uint32_t input_last_activity_ms(void);

#ifdef __cplusplus
}
#endif

#endif // INPUT_MANAGER_H
