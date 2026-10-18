// Copyright (c) 2026 High Boy FLAC iPod contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Link stubs for the TentacleOS functions the app calls. Their declarations
// come from the real TentacleOS headers, so any signature mismatch fails here.

#include "audio_i2s.h"
#include "battery_service.h"
#include "input_manager.h"
#include "sys_time.h"
#include "ui_manager.h"

bool input_is_down(input_button_t button) {
  (void)button;
  return false;
}

void ui_switch_screen(screen_id_t new_screen) {
  (void)new_screen;
}

void ui_screen_load_owned(lv_obj_t **slot, lv_obj_t *scr) {
  *slot = scr;
  lv_screen_load(scr);
}

void ui_input_set_screen_handler(ui_input_handler_t handler, void *ctx) {
  (void)handler;
  (void)ctx;
}

void audio_i2s_set_volume(uint8_t pct) {
  (void)pct;
}

esp_err_t audio_i2s_stream_start(uint32_t sample_rate) {
  (void)sample_rate;
  return ESP_OK;
}

int audio_i2s_stream_write(const int16_t *pcm, int n_samples) {
  (void)pcm;
  return n_samples;
}

void audio_i2s_stream_stop(void) {}

void audio_click(void) {}

int battery_service_soc(void) {
  return 50;
}

bool battery_service_get(battery_snapshot_t *out) {
  (void)out;
  return false;
}

time_t sys_time_now(void) {
  return 0;
}

sys_time_state_t sys_time_state(void) {
  return SYS_TIME_STATE_ESTIMATED;
}
