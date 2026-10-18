// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// input_manager + ui_manager for the simulator, following TentacleOS:
//  - input_manager: debounced PRESS/RELEASE, REPEAT after 400 ms every 120 ms,
//    one LONG_PRESS at 800 ms (Drivers/input_manager).
//  - ui_manager: one pump dispatching events to the active screen's handler,
//    screen switching with close hooks and a 500 ms input lock
//    (Applications/ui/ui_manager.c).

#include "hb_ui.h"

#include <string.h>

#include "esp_log.h"
#include "input_manager.h"
#include "ui_ipod.h"
#include "ui_manager.h"

#define LONG_PRESS_MS    800
#define REPEAT_DELAY_MS  400
#define REPEAT_PERIOD_MS 120
#define EVENT_QUEUE_LEN  16
#define INPUT_LOCK_MS    500

typedef struct {
  bool is_down;
  bool is_long_fired;
  uint32_t since_ms;
  uint32_t next_repeat_ms;
} btn_state_t;

static const char *TAG = "HB_UI";

static btn_state_t s_btn[INPUT_BTN_COUNT];
static input_event_t s_queue[EVENT_QUEUE_LEN];
static int s_q_head;
static int s_q_len;
static uint32_t s_now_ms;
static uint32_t s_last_activity_ms;

static screen_id_t s_current = SCREEN_NONE;
static ui_input_handler_t s_handler;
static void *s_handler_ctx;
static uint32_t s_input_lock_until;
static lv_obj_t *s_launcher;
static bool s_quit_requested;

static void push_event(input_button_t b, input_action_t a) {
  if (s_q_len == EVENT_QUEUE_LEN)
    return; // the real queue drops on overflow too
  s_queue[(s_q_head + s_q_len) % EVENT_QUEUE_LEN] =
      (input_event_t){.button = b, .action = a, .timestamp_ms = s_now_ms};
  s_q_len++;
}

void hb_input_set(input_button_t b, bool down, uint32_t now_ms) {
  if (b >= INPUT_BTN_COUNT)
    return;
  s_now_ms = now_ms;
  btn_state_t *st = &s_btn[b];
  if (down == st->is_down)
    return;
  st->is_down = down;
  s_last_activity_ms = now_ms;
  if (down) {
    st->since_ms = now_ms;
    st->next_repeat_ms = now_ms + REPEAT_DELAY_MS;
    st->is_long_fired = false;
    push_event(b, INPUT_ACTION_PRESS);
  } else {
    push_event(b, INPUT_ACTION_RELEASE);
  }
}

void hb_input_tick(uint32_t now_ms) {
  s_now_ms = now_ms;
  for (int b = 0; b < INPUT_BTN_COUNT; b++) {
    btn_state_t *st = &s_btn[b];
    if (!st->is_down)
      continue;
    if (!st->is_long_fired && now_ms - st->since_ms >= LONG_PRESS_MS) {
      st->is_long_fired = true;
      push_event((input_button_t)b, INPUT_ACTION_LONG_PRESS);
    }
    while ((int32_t)(now_ms - st->next_repeat_ms) >= 0) {
      push_event((input_button_t)b, INPUT_ACTION_REPEAT);
      st->next_repeat_ms += REPEAT_PERIOD_MS;
    }
  }
}

esp_err_t input_manager_init(void) {
  return ESP_OK;
}

bool input_get_event(input_event_t *out_event, uint32_t timeout_ms) {
  (void)timeout_ms;
  if (s_q_len == 0)
    return false;
  *out_event = s_queue[s_q_head];
  s_q_head = (s_q_head + 1) % EVENT_QUEUE_LEN;
  s_q_len--;
  return true;
}

bool input_is_down(input_button_t button) {
  return button < INPUT_BTN_COUNT && s_btn[button].is_down;
}

uint32_t input_last_activity_ms(void) {
  return s_last_activity_ms;
}

// ---------------------------------------------------------------------------

bool ui_acquire(void) {
  return true; // the simulator runs LVGL on a single thread
}

void ui_release(void) {}

void ui_async_call(lv_async_cb_t cb, void *user_data) {
  lv_async_call(cb, user_data);
}

bool ui_input_is_locked(void) {
  return (int32_t)(lv_tick_get() - s_input_lock_until) < 0;
}

void ui_input_set_screen_handler(ui_input_handler_t handler, void *ctx) {
  s_handler = handler;
  s_handler_ctx = ctx;
}

void hb_ui_pump(void) {
  input_event_t ev;
  while (input_get_event(&ev, 0)) {
    if (s_handler == NULL || ui_input_is_locked())
      continue;
    ui_input_handler_t handler = s_handler;
    handler(&ev, s_handler_ctx);
    if (s_handler != handler)
      break;
  }
}

static void screen_slot_clear_cb(lv_event_t *e) {
  lv_obj_t **slot = lv_event_get_user_data(e);
  if (slot != NULL)
    *slot = NULL;
}

void ui_screen_load_owned(lv_obj_t **slot, lv_obj_t *scr) {
  if (slot != NULL)
    *slot = scr;
  lv_obj_add_event_cb(scr, screen_slot_clear_cb, LV_EVENT_DELETE, slot);
  lv_screen_load(scr);
}

screen_id_t ui_current_screen(void) {
  return s_current;
}

// Stand-in for TentacleOS's APPS menu, so leaving and re-entering the app can
// be exercised.
static void launcher_input(const input_event_t *ev, void *ctx) {
  (void)ctx;
  if (ev->action != INPUT_ACTION_PRESS)
    return;
  if (ev->button == INPUT_BTN_OK || ev->button == INPUT_BTN_RIGHT)
    ui_switch_screen(SCREEN_IPOD);
  else if (ev->button == INPUT_BTN_BACK)
    s_quit_requested = true;
}

static void launcher_open(void) {
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0614), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "APPS");
  lv_obj_set_style_text_color(title, lv_color_hex(0xB89AFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  lv_obj_t *card = lv_obj_create(scr);
  lv_obj_set_size(card, 120, 120);
  lv_obj_center(card);
  lv_obj_set_style_radius(card, 24, 0);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x7A52D6), 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *icon = lv_label_create(card);
  lv_label_set_text(icon, LV_SYMBOL_AUDIO);
  lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, 0);
  lv_obj_center(icon);

  lv_obj_t *name = lv_label_create(scr);
  lv_label_set_text(name, LV_SYMBOL_LEFT "  iPod  " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
  lv_obj_align(name, LV_ALIGN_CENTER, 0, 82);

  lv_obj_t *hint = lv_label_create(scr);
  lv_label_set_text(hint, "OK open    BACK quit simulator");
  lv_obj_set_style_text_color(hint, lv_color_hex(0x8A80A0), 0);
  lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

  ui_input_set_screen_handler(launcher_input, NULL);
  ui_screen_load_owned(&s_launcher, scr);
}

void ui_switch_screen(screen_id_t new_screen) {
  void (*open_fn)(void) = NULL;
  if (new_screen == SCREEN_GAMES_MENU)
    open_fn = launcher_open;
  else if (new_screen == SCREEN_IPOD)
    open_fn = ui_ipod_open;
  if (open_fn == NULL) {
    ESP_LOGW(TAG, "Screen %d not available - staying put", (int)new_screen);
    return;
  }
  s_input_lock_until = lv_tick_get() + INPUT_LOCK_MS;
  lv_obj_t *outgoing = lv_screen_active();
  if (s_current == SCREEN_IPOD)
    ui_ipod_stop(); // screen_close_fn(SCREEN_IPOD)
  ui_input_set_screen_handler(NULL, NULL);
  open_fn();
  s_current = new_screen;
  if (outgoing != NULL && outgoing != lv_screen_active() && lv_obj_is_valid(outgoing))
    lv_obj_delete_async(outgoing);
}

bool hb_ui_quit_requested(void) {
  return s_quit_requested;
}
