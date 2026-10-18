// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// The iPod app's screen for TentacleOS: an iPod classic (colour) style UI for
// the FLAC library on the microSD card.
//
// Controls (High Boy buttons -> iPod click wheel):
//   UP / DOWN      scroll wheel: move through lists; in Now Playing UP / DOWN
//                  raise / lower the volume (or scrub / rate / shuffle after
//                  pressing OK there)
//   OK             centre button: select; in Now Playing cycle the wheel mode
//   OK (hold)      add the highlighted song / album / artist to On-The-Go
//   BACK           MENU: back one level (leave the app from the main menu)
//   BACK (hold)    PLAY/PAUSE
//   LEFT / RIGHT   previous / next song; hold to rewind / fast-forward
//   OK + BACK hold HOLD switch: lock / unlock the controls

#include "ui_ipod.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys_prio.h"
#include "vfs_config.h"

#include "input_manager.h"
#include "ipod_art.h"
#include "ipod_dsp.h"
#include "ipod_library.h"
#include "ipod_menu.h"
#include "ipod_player.h"
#include "ipod_port.h"
#include "ipod_settings.h"
#include "ipod_text.h"
#include "lvgl.h"
// TentacleOS runs LVGL with a 1 MB image cache keyed by source address: art
// buffers must be dropped from it before they are freed (as assets_manager does).
#include "misc/cache/instance/lv_image_cache.h"
#include "ui_manager.h"
#include "ui_metrics.h"

// ---------------------------------------------------------------------------
// Storage

#define IPOD_DIR      VFS_MOUNT_POINT "/.ipod"
#define CACHE_PATH    IPOD_DIR "/library.db"
#define SETTINGS_PATH IPOD_DIR "/settings.cfg"
#define OTG_PATH      IPOD_DIR "/On-The-Go.m3u"
#define MUSIC_DIR     VFS_MOUNT_POINT "/Music"

// ---------------------------------------------------------------------------
// Look: iPod classic (colour) palette

#define C_BG          0xFFFFFF
#define C_TEXT        0x000000
#define C_SUBTEXT     0x5C5C5C
#define C_HEADER_TOP  0xFDFDFD
#define C_HEADER_BOT  0xC6CBD2
#define C_HEADER_LINE 0x7C838C
#define C_SEL_TOP     0x6EADF4
#define C_SEL_BOT     0x2463D1
#define C_SEL_TEXT    0xFFFFFF
#define C_ARROW       0x4A4A4A
#define C_BLUE        0x2C6BD6
#define C_BAR_TOP     0xF7F9FC
#define C_BAR_BOT     0xC9D3E2
#define C_BAR_LINE    0x7D8FAD
#define C_FILL_TOP    0x8EC2F9
#define C_FILL_BOT    0x2C6BD6
#define C_BATT_LINE   0x3F3F3F
#define C_BATT_TOP    0xB2F27A
#define C_BATT_BOT    0x45A622
#define C_BATT_LOW_T  0xF7A0A0
#define C_BATT_LOW_B  0xC72424
#define C_ART_PH_TOP  0xF0F0F0
#define C_ART_PH_BOT  0xB9BDC4
#define C_STAR        0x2C6BD6
#define C_STAR_EMPTY  0xB8BEC8
#define C_OVERLAY     0x202020
#define BG_RGB565     0xFFFF

// ---------------------------------------------------------------------------
// Geometry and timing

#define HEADER_H          24
#define ROW_H             26
#define ROW_PAD_X         8
#define SCROLLBAR_W       8
#define MAX_VIS_ROWS      24
#define REFRESH_MS        100
#define ANIM_MS           170
#define BATT_W            23
#define BATT_H            12
#define LETTER_BOX        64
#define LETTER_SCALE      768 // 3x (256 = 1x)
#define LETTER_HIDE_MS    700
#define LETTER_MIN_REPEAT 3
#define TOAST_MS          1500
#define VOLUME_SHOW_MS    2000
#define MODE_TIMEOUT_MS   6000
#define VOLUME_STEP       4
#define STAR_PX           16
#define STAR_GAP          6
#define RATING_STARS      5
#define SEARCH_MAX        24
#define SEARCH_HITS_MAX   200
#define KEYS_VISIBLE      9
#define KEY_W             24
#define CLOCK_SCALE       768
#define STOPWATCH_SCALE   512
#define STOPWATCH_LAPS    4
#define SCAN_TASK_STACK   12288
#define SCAN_WAIT_MS      3000
#define NO_GROUP          0u

// Wheel acceleration (in rows / ms) by number of auto-repeats.
#define ACCEL_START_REPEATS 4
#define ACCEL_MAX_SHIFT     6
#define ACCEL_LIST_FRACTION 20
#define SEEK_STEP_1_MS      1500
#define SEEK_STEP_2_MS      3000
#define SEEK_STEP_3_MS      6000
#define SEEK_REPEATS_1      10
#define SEEK_REPEATS_2      25
#define SCRUB_STEP_1_MS     2000
#define SCRUB_STEP_2_MS     5000
#define SCRUB_STEP_3_MS     10000

static const char *TAG = "UI_IPOD";

// Search keyboard: letters, digits, space, delete (the iPod's letter strip).
static const char SEARCH_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 \b";
#define SEARCH_KEY_COUNT ((int)(sizeof(SEARCH_KEYS) - 1))
#define SEARCH_KEY_DEL   '\b'

typedef enum {
  WHEEL_VOLUME = 0, // normal Now Playing: the wheel sets the volume
  WHEEL_SCRUB,
  WHEEL_RATING,
  WHEEL_SHUFFLE,
  WHEEL_MODE_COUNT,
} wheel_mode_t;

typedef struct {
  lv_obj_t *obj;
  lv_obj_t *text;
  lv_obj_t *right;
  char text_cache[IPOD_ROW_TEXT_MAX];
  char right_cache[IPOD_ROW_RIGHT_MAX];
  int state_cache; ///< -1 unknown, else selected | shown << 1
} row_w_t;

typedef struct {
  bool is_open;
  lv_timer_t *timer;
  int32_t w;
  int32_t h;
  int32_t body_h;
  bool is_landscape;

  // header
  lv_obj_t *title;
  lv_obj_t *play_icon;
  lv_obj_t *lock_icon;
  lv_obj_t *batt_fill;
  lv_obj_t *batt_bolt;

  // pages
  lv_obj_t *page;
  lv_obj_t *old_page;

  // list page
  row_w_t rows[MAX_VIS_ROWS];
  int vis_rows;
  int32_t row_w; ///< kept here: LVGL reports 0 until its first layout pass
  lv_obj_t *sb_track;
  lv_obj_t *sb_thumb;

  // overlays
  lv_obj_t *letter_box;
  lv_obj_t *letter_label;
  uint32_t letter_until;
  char letter_shown;
  lv_obj_t *toast;
  lv_obj_t *toast_label;
  uint32_t toast_until;

  // now playing page
  lv_obj_t *np_counter;
  lv_obj_t *np_art;
  lv_obj_t *np_art_ph;
  lv_obj_t *np_refl;
  lv_obj_t *np_title;
  lv_obj_t *np_artist;
  lv_obj_t *np_album;
  lv_obj_t *np_bar;
  lv_obj_t *np_fill;
  lv_obj_t *np_knob;
  lv_obj_t *np_elapsed;
  lv_obj_t *np_remain;
  lv_obj_t *np_vol;
  lv_obj_t *np_vol_fill;
  lv_obj_t *np_stars[RATING_STARS];
  lv_obj_t *np_mode_label;
  int32_t np_bar_w;
  int32_t np_bar_y;
  int32_t np_vol_bar_w;
  int32_t art_box;
  int32_t refl_h;
  uint32_t np_track_shown;
  uint32_t np_seq_shown;
  wheel_mode_t wheel_mode;
  uint32_t mode_until;
  uint32_t volume_until;

  // album art
  ipod_art_image_t art;
  uint32_t art_req;
  uint32_t art_track;
  bool has_art;
  lv_image_dsc_t art_dsc;
  lv_image_dsc_t refl_dsc;

  // search page
  char query[SEARCH_MAX + 1];
  int key_idx;
  bool is_results_focus;
  ipod_hit_t *hits;
  size_t nhits;
  int hit_sel;
  int hit_top;
  lv_obj_t *sr_query;
  lv_obj_t *sr_count;
  lv_obj_t *sr_keys[KEYS_VISIBLE];

  // clock / stopwatch
  lv_obj_t *clk_time;
  lv_obj_t *clk_date;
  lv_obj_t *sw_time;
  lv_obj_t *sw_laps[STOPWATCH_LAPS];
  bool is_sw_running;
  uint32_t sw_start_ms;
  uint32_t sw_accum_ms;
  uint32_t sw_lap_ms[STOPWATCH_LAPS];
  int sw_nlaps;

  // library update
  lv_obj_t *scan_count;
  struct scan_ctx *scan; ///< non-NULL while a scan task runs
  bool is_scan_modal;

  // data
  ipod_library_t *lib;
  ipod_library_t *pending_lib;
  ipod_settings_t settings;
  ipod_menu_t menu;
  ipod_player_status_t st;
  uint32_t last_end_seq;
  uint32_t list_track_seq;
  bool is_cache_dirty;

  // input
  bool is_ok_long;
  bool is_back_long;
  bool is_ok_down;   ///< from the event stream: input_is_down() reports the
  bool is_back_down; ///< live pins, which run ahead of still-queued events
  bool is_chord;
  bool is_seeking;
  int seek_repeats;
  int wheel_repeats;
  bool is_hold;
} ui_state_t;

// Owned by the scan task until it signals `exited`; if a scan ever hangs past
// shutdown it is abandoned (leaked) rather than freed under the task's feet.
typedef struct scan_ctx {
  ipod_scan_progress_t prog;
  ipod_library_t *prev;
  ipod_library_t *result;
  SemaphoreHandle_t exited;
  char root[64];
} scan_ctx_t;

static ui_state_t s;
static lv_obj_t *s_screen; // survives stop(): the owned-slot cleared on delete
static screen_id_t s_return = SCREEN_GAMES_MENU;

static uint16_t s_star_full[STAR_PX * STAR_PX];
static uint16_t s_star_empty[STAR_PX * STAR_PX];
static lv_image_dsc_t s_star_full_dsc;
static lv_image_dsc_t s_star_empty_dsc;

static void build_page(int dir);
static void list_render(void);
static void search_render(void);
static void np_update(uint32_t now);
static void start_scan(bool is_modal);
static void show_toast(const char *text);
static void leave_app(void);

// ---------------------------------------------------------------------------
// Small helpers

static lv_obj_t *plain_obj(lv_obj_t *parent) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void set_vgrad(lv_obj_t *o, uint32_t top, uint32_t bottom) {
  lv_obj_set_style_bg_color(o, lv_color_hex(top), 0);
  lv_obj_set_style_bg_grad_color(o, lv_color_hex(bottom), 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, "");
  return l;
}

static void set_label_if_changed(lv_obj_t *l, const char *text) {
  if (l != NULL && strcmp(lv_label_get_text(l), text) != 0)
    lv_label_set_text(l, text);
}

static void set_hidden(lv_obj_t *o, bool hidden) {
  if (o == NULL)
    return;
  if (hidden)
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static ipod_view_t *top_view(void) {
  return ipod_menu_top(&s.menu);
}

static bool is_list_view(const ipod_view_t *v) {
  return v != NULL && v->kind < IPOD_VIEW_NOW_PLAYING;
}

static void click(void) {
  if (s.settings.clicker)
    ipod_player_click();
}

static void image_dsc_init(lv_image_dsc_t *d, const uint16_t *px, uint16_t w, uint16_t h) {
  memset(d, 0, sizeof(*d));
  d->header.magic = LV_IMAGE_HEADER_MAGIC;
  d->header.cf = LV_COLOR_FORMAT_RGB565;
  d->header.w = w;
  d->header.h = h;
  d->header.stride = (uint32_t)w * sizeof(uint16_t);
  d->data = (const uint8_t *)px;
  d->data_size = (uint32_t)w * h * sizeof(uint16_t);
}

// Anti-aliased 5-point star rendered once into an RGB565 buffer (the device
// fonts have no star glyph).
static void render_star(uint16_t *px, uint32_t rgb, bool is_dot) {
  const float cx = STAR_PX / 2.0f;
  const float cy = STAR_PX / 2.0f + 0.5f;
  const float r_out = STAR_PX / 2.0f - 0.5f;
  const float r_in = r_out * 0.42f;
  float vx[10];
  float vy[10];
  for (int i = 0; i < 10; i++) {
    float a = -1.5707963f + (float)i * 0.6283185f;
    float r = (i % 2 == 0) ? r_out : r_in;
    vx[i] = cx + r * cosf(a);
    vy[i] = cy + r * sinf(a);
  }
  const uint32_t r8 = (rgb >> 16) & 0xFF;
  const uint32_t g8 = (rgb >> 8) & 0xFF;
  const uint32_t b8 = rgb & 0xFF;
  for (int y = 0; y < STAR_PX; y++) {
    for (int x = 0; x < STAR_PX; x++) {
      int inside = 0;
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          const float px_ = (float)x + (sx + 0.5f) / 4.0f;
          const float py_ = (float)y + (sy + 0.5f) / 4.0f;
          bool in = false;
          if (is_dot) {
            const float dx = px_ - cx;
            const float dy = py_ - cy;
            in = dx * dx + dy * dy <= 2.6f * 2.6f;
          } else {
            for (int i = 0, j = 9; i < 10; j = i++) {
              if (((vy[i] > py_) != (vy[j] > py_)) &&
                  (px_ < (vx[j] - vx[i]) * (py_ - vy[i]) / (vy[j] - vy[i]) + vx[i]))
                in = !in;
            }
          }
          inside += in ? 1 : 0;
        }
      }
      const uint32_t a = (uint32_t)inside * 255u / 16u;
      const uint32_t r = (r8 * a + 255u * (255u - a)) / 255u;
      const uint32_t g = (g8 * a + 255u * (255u - a)) / 255u;
      const uint32_t b = (b8 * a + 255u * (255u - a)) / 255u;
      px[y * STAR_PX + x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
}

static lv_obj_t *mk_lock_icon(lv_obj_t *parent, uint32_t color, int32_t scale) {
  lv_obj_t *box = plain_obj(parent);
  lv_obj_set_size(box, 10 * scale, 13 * scale);
  lv_obj_t *shackle = plain_obj(box);
  lv_obj_set_size(shackle, 8 * scale, 9 * scale);
  lv_obj_set_pos(shackle, scale, 0);
  lv_obj_set_style_border_width(shackle, 2 * scale, 0);
  lv_obj_set_style_border_color(shackle, lv_color_hex(color), 0);
  lv_obj_set_style_radius(shackle, 4 * scale, 0);
  lv_obj_t *body = plain_obj(box);
  lv_obj_set_size(body, 10 * scale, 7 * scale);
  lv_obj_set_pos(body, 0, 6 * scale);
  lv_obj_set_style_bg_color(body, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(body, scale, 0);
  return box;
}

// ---------------------------------------------------------------------------
// Storage

static void save_otg(void) {
  if (!ipod_lib_write_m3u(s.lib, OTG_PATH, s.menu.otg, s.menu.otg_count))
    ESP_LOGW(TAG, "could not save On-The-Go");
}

static void load_otg(void) {
  uint32_t *tracks = NULL;
  size_t n = ipod_lib_read_m3u(s.lib, OTG_PATH, &tracks);
  ipod_menu_set_otg(&s.menu, tracks, n);
  free(tracks);
}

static void save_settings(void) {
  if (!ipod_settings_save(&s.settings, SETTINGS_PATH))
    ESP_LOGW(TAG, "could not save settings");
}

static void save_cache(void) {
  if (ipod_lib_save(s.lib, CACHE_PATH))
    s.is_cache_dirty = false;
  else
    ESP_LOGW(TAG, "could not save the library cache");
}

static void apply_settings(void) {
  if (s.settings.volume > s.settings.volume_limit)
    s.settings.volume = s.settings.volume_limit;
  ipod_player_set_volume(s.settings.volume);
  ipod_player_set_shuffle(s.settings.shuffle);
  ipod_player_set_repeat(s.settings.repeat);
  ipod_player_set_eq(s.settings.eq_preset);
  ipod_player_set_sound_check(s.settings.sound_check);
}

// ---------------------------------------------------------------------------
// Header

static void header_create(lv_obj_t *scr) {
  lv_obj_t *hdr = plain_obj(scr);
  lv_obj_set_size(hdr, s.w, HEADER_H);
  set_vgrad(hdr, C_HEADER_TOP, C_HEADER_BOT);
  lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_width(hdr, 1, 0);
  lv_obj_set_style_border_color(hdr, lv_color_hex(C_HEADER_LINE), 0);

  s.title = mk_label(hdr, &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_width(s.title, s.w - 96);
  lv_label_set_long_mode(s.title, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(s.title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s.title, LV_ALIGN_CENTER, 0, 0);

  s.play_icon = mk_label(hdr, &lv_font_montserrat_12, C_BLUE);
  lv_obj_align(s.play_icon, LV_ALIGN_LEFT_MID, 8, 0);
  set_hidden(s.play_icon, true);

  s.lock_icon = mk_lock_icon(hdr, C_ARROW, 1);
  lv_obj_align(s.lock_icon, LV_ALIGN_LEFT_MID, 26, 0);
  set_hidden(s.lock_icon, true);

  lv_obj_t *batt = plain_obj(hdr);
  lv_obj_set_size(batt, BATT_W, BATT_H);
  lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -10, 0);
  lv_obj_set_style_border_width(batt, 1, 0);
  lv_obj_set_style_border_color(batt, lv_color_hex(C_BATT_LINE), 0);
  lv_obj_set_style_radius(batt, 2, 0);
  lv_obj_set_style_bg_color(batt, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(batt, LV_OPA_COVER, 0);
  lv_obj_t *tip = plain_obj(hdr);
  lv_obj_set_size(tip, 2, 5);
  lv_obj_align_to(tip, batt, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(tip, lv_color_hex(C_BATT_LINE), 0);
  lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
  s.batt_fill = plain_obj(batt);
  lv_obj_set_pos(s.batt_fill, 2, 2);
  lv_obj_set_height(s.batt_fill, BATT_H - 4);
  s.batt_bolt = mk_label(hdr, &lv_font_montserrat_12, C_BATT_LINE);
  lv_label_set_text(s.batt_bolt, LV_SYMBOL_CHARGE);
  lv_obj_align_to(s.batt_bolt, batt, LV_ALIGN_OUT_LEFT_MID, -3, 0);
}

static void header_update(void) {
  const ipod_view_t *v = top_view();
  const char *title = s.is_scan_modal ? "Updating Library" : v != NULL ? v->title : "iPod";
  set_label_if_changed(s.title, title);

  const bool playing = s.st.state == IPOD_PLAYER_PLAYING;
  const bool paused = s.st.state == IPOD_PLAYER_PAUSED;
  set_hidden(s.play_icon, !(playing || paused));
  set_label_if_changed(s.play_icon, playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  set_hidden(s.lock_icon, !s.is_hold);
  if (s.is_hold)
    lv_obj_align(s.lock_icon, LV_ALIGN_LEFT_MID, (playing || paused) ? 26 : 8, 0);

  int pct = ipod_port_battery_percent();
  if (pct < 0)
    pct = 100;
  if (pct > 100)
    pct = 100;
  const int32_t fill_w = (BATT_W - 4) * pct / 100;
  lv_obj_set_width(s.batt_fill, fill_w < 1 ? 1 : fill_w);
  if (pct <= 20)
    set_vgrad(s.batt_fill, C_BATT_LOW_T, C_BATT_LOW_B);
  else
    set_vgrad(s.batt_fill, C_BATT_TOP, C_BATT_BOT);
  set_hidden(s.batt_bolt, !ipod_port_is_charging());
}

// ---------------------------------------------------------------------------
// Overlays: fast-scroll letter and toasts

static void overlays_create(lv_obj_t *scr) {
  s.letter_box = plain_obj(scr);
  lv_obj_set_size(s.letter_box, LETTER_BOX, LETTER_BOX);
  lv_obj_align(s.letter_box, LV_ALIGN_CENTER, 0, HEADER_H / 2);
  lv_obj_set_style_radius(s.letter_box, 10, 0);
  lv_obj_set_style_bg_color(s.letter_box, lv_color_hex(C_OVERLAY), 0);
  lv_obj_set_style_bg_opa(s.letter_box, LV_OPA_70, 0);
  s.letter_label = mk_label(s.letter_box, &lv_font_montserrat_16, C_SEL_TEXT);
  lv_obj_center(s.letter_label);
  lv_obj_set_style_transform_scale(s.letter_label, LETTER_SCALE, 0);
  lv_obj_set_style_transform_pivot_x(s.letter_label, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(s.letter_label, lv_pct(50), 0);
  set_hidden(s.letter_box, true);

  s.toast = plain_obj(scr);
  lv_obj_set_size(s.toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(s.toast, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_style_radius(s.toast, 10, 0);
  lv_obj_set_style_bg_color(s.toast, lv_color_hex(C_OVERLAY), 0);
  lv_obj_set_style_bg_opa(s.toast, LV_OPA_80, 0);
  lv_obj_set_style_pad_hor(s.toast, 12, 0);
  lv_obj_set_style_pad_ver(s.toast, 8, 0);
  s.toast_label = mk_label(s.toast, &lv_font_montserrat_14, C_SEL_TEXT);
  set_hidden(s.toast, true);
}

static void show_toast(const char *text) {
  lv_label_set_text(s.toast_label, text);
  set_hidden(s.toast, false);
  lv_obj_move_foreground(s.toast);
  s.toast_until = lv_tick_get() + TOAST_MS;
}

static void show_letter(char letter) {
  char txt[2] = {letter, '\0'};
  set_label_if_changed(s.letter_label, txt);
  set_hidden(s.letter_box, false);
  lv_obj_move_foreground(s.letter_box);
  s.letter_shown = letter;
  s.letter_until = lv_tick_get() + LETTER_HIDE_MS;
}

// ---------------------------------------------------------------------------
// Rows (shared by menu lists and search results)

static void rows_create(lv_obj_t *parent, int32_t y, int32_t height, int32_t width) {
  s.row_w = width;
  s.vis_rows = (int)(height / ROW_H);
  if (s.vis_rows > MAX_VIS_ROWS)
    s.vis_rows = MAX_VIS_ROWS;
  for (int i = 0; i < s.vis_rows; i++) {
    row_w_t *r = &s.rows[i];
    memset(r, 0, sizeof(*r));
    r->state_cache = -1;
    r->obj = plain_obj(parent);
    lv_obj_set_size(r->obj, width, ROW_H);
    lv_obj_set_pos(r->obj, 0, y + i * ROW_H);
    set_vgrad(r->obj, C_SEL_TOP, C_SEL_BOT);
    lv_obj_set_style_bg_opa(r->obj, LV_OPA_TRANSP, 0);
    r->right = mk_label(r->obj, &lv_font_montserrat_14, C_SUBTEXT);
    lv_obj_align(r->right, LV_ALIGN_RIGHT_MID, -ROW_PAD_X, 0);
    r->text = mk_label(r->obj, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(r->text, width - 2 * ROW_PAD_X - 20);
    lv_label_set_long_mode(r->text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(r->text, LV_ALIGN_LEFT_MID, ROW_PAD_X, 0);
    set_hidden(r->obj, true);
  }
}

static void row_set(row_w_t *r,
                    bool shown,
                    const char *text,
                    const char *right,
                    ipod_row_icon_t icon,
                    bool selected,
                    int32_t width) {
  const int state = (selected ? 1 : 0) | (shown ? 2 : 0);
  set_hidden(r->obj, !shown);
  if (!shown) {
    r->state_cache = state;
    return;
  }
  char right_buf[IPOD_ROW_RIGHT_MAX];
  switch (icon) {
    case IPOD_ICON_ARROW:
      ipod_text_copy(right_buf, sizeof(right_buf), LV_SYMBOL_RIGHT);
      break;
    case IPOD_ICON_CHECK:
      ipod_text_copy(right_buf, sizeof(right_buf), LV_SYMBOL_OK);
      break;
    case IPOD_ICON_SPEAKER:
      ipod_text_copy(right_buf, sizeof(right_buf), LV_SYMBOL_VOLUME_MAX);
      break;
    default:
      ipod_text_copy(right_buf, sizeof(right_buf), right != NULL ? right : "");
      break;
  }
  const bool state_changed = state != r->state_cache;
  if (state_changed) {
    lv_obj_set_style_bg_opa(r->obj, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(r->text, lv_color_hex(selected ? C_SEL_TEXT : C_TEXT), 0);
    // The highlighted row scrolls a long title, as the iPod does.
    lv_label_set_long_mode(r->text,
                           selected ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR : LV_LABEL_LONG_MODE_DOTS);
  }
  if (state_changed || strcmp(r->right_cache, right_buf) != 0) {
    ipod_text_copy(r->right_cache, sizeof(r->right_cache), right_buf);
    lv_label_set_text(r->right, right_buf);
    const bool is_symbol = icon != IPOD_ICON_NONE;
    lv_obj_set_style_text_font(
        r->right, is_symbol ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
    uint32_t color = selected ? C_SEL_TEXT
                              : (icon == IPOD_ICON_SPEAKER ? C_BLUE
                                 : is_symbol               ? C_ARROW
                                                           : C_SUBTEXT);
    lv_obj_set_style_text_color(r->right, lv_color_hex(color), 0);
    lv_obj_update_layout(r->right);
    int32_t right_w = right_buf[0] != '\0' ? lv_obj_get_width(r->right) + 6 : 0;
    lv_obj_set_width(r->text, width - 2 * ROW_PAD_X - right_w);
  }
  if (state_changed || strcmp(r->text_cache, text) != 0) {
    ipod_text_copy(r->text_cache, sizeof(r->text_cache), text);
    lv_label_set_text(r->text, text);
  }
  r->state_cache = state;
}

static void scrollbar_create(lv_obj_t *parent, int32_t y, int32_t height) {
  s.sb_track = plain_obj(parent);
  lv_obj_set_size(s.sb_track, SCROLLBAR_W, height);
  lv_obj_set_pos(s.sb_track, s.w - SCROLLBAR_W, y);
  lv_obj_set_style_bg_color(s.sb_track, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s.sb_track, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(s.sb_track, LV_BORDER_SIDE_LEFT, 0);
  lv_obj_set_style_border_width(s.sb_track, 1, 0);
  lv_obj_set_style_border_color(s.sb_track, lv_color_hex(C_HEADER_BOT), 0);
  s.sb_thumb = plain_obj(s.sb_track);
  lv_obj_set_width(s.sb_thumb, SCROLLBAR_W - 3);
  lv_obj_set_x(s.sb_thumb, 2);
  lv_obj_set_style_radius(s.sb_thumb, 3, 0);
  set_vgrad(s.sb_thumb, C_SEL_TOP, C_SEL_BOT);
}

static void scrollbar_update(size_t total, int top, int vis, int32_t height) {
  if (s.sb_track == NULL)
    return;
  const bool needed = (int)total > vis;
  set_hidden(s.sb_track, !needed);
  if (!needed)
    return;
  int32_t thumb_h = (int32_t)((int64_t)height * vis / (int64_t)total);
  if (thumb_h < 12)
    thumb_h = 12;
  int32_t max_top = (int32_t)total - vis;
  int32_t y = max_top > 0 ? (int32_t)((int64_t)(height - thumb_h) * top / max_top) : 0;
  lv_obj_set_height(s.sb_thumb, thumb_h);
  lv_obj_set_y(s.sb_thumb, y);
}

// ---------------------------------------------------------------------------
// List page (all menus)

static void list_create(lv_obj_t *page) {
  const ipod_view_t *v = top_view();
  const int fit = (int)(s.body_h / ROW_H);
  const bool needs_bar = v != NULL && (int)v->nrows > fit;
  const int32_t width = needs_bar ? s.w - SCROLLBAR_W : s.w;
  rows_create(page, 0, s.body_h, width);
  scrollbar_create(page, 0, s.body_h);
  ipod_menu_move(&s.menu, 0, s.vis_rows); // keep the selection visible
  list_render();
}

static void list_render(void) {
  ipod_view_t *v = top_view();
  if (v == NULL || s.vis_rows == 0 || !is_list_view(v))
    return;
  const uint32_t playing = s.st.has_track ? s.st.track : IPOD_NO_TRACK;
  const int32_t width = s.row_w;
  const bool hide_highlight = v->kind == IPOD_VIEW_ABOUT;
  for (int i = 0; i < s.vis_rows; i++) {
    const size_t idx = (size_t)(v->top + i);
    if (idx >= v->nrows) {
      row_set(&s.rows[i], false, "", "", IPOD_ICON_NONE, false, width);
      continue;
    }
    ipod_row_text_t rt;
    ipod_menu_row_text(&s.menu, v, idx, playing, &rt);
    const bool selected = !hide_highlight && rt.is_selectable && (int)idx == v->sel;
    row_set(&s.rows[i], true, rt.text, rt.right, rt.icon, selected, width);
  }
  scrollbar_update(v->nrows, v->top, s.vis_rows, s.body_h);
  s.list_track_seq = s.st.track_seq;
}

// ---------------------------------------------------------------------------
// Now Playing page

static void art_release(void) {
  if (s.art.pixels != NULL)
    lv_image_cache_drop(&s.art_dsc);
  if (s.art.reflection != NULL)
    lv_image_cache_drop(&s.refl_dsc);
  ipod_art_image_free(&s.art);
  s.has_art = false;
}

static void np_show_art(void) {
  if (s.np_art == NULL)
    return;
  const bool has = s.has_art && s.art.pixels != NULL;
  set_hidden(s.np_art, !has);
  set_hidden(s.np_refl, !has || s.art.reflection == NULL);
  set_hidden(s.np_art_ph, has);
  if (!has)
    return;
  lv_image_set_src(s.np_art, &s.art_dsc);
  lv_obj_set_size(s.np_art, s.art.w, s.art.h);
  if (s.art.reflection != NULL) {
    lv_image_set_src(s.np_refl, &s.refl_dsc);
    lv_obj_set_size(s.np_refl, s.art.w, s.art.refl_h);
  }
  // Centre non-square art inside the art box.
  lv_obj_t *box = lv_obj_get_parent(s.np_art);
  lv_obj_align(s.np_art, LV_ALIGN_TOP_MID, 0, (s.art_box - s.art.h) / 2);
  lv_obj_align_to(s.np_refl, s.np_art, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
  (void)box;
}

static void art_request_for(uint32_t track) {
  const ipod_track_t *t = ipod_lib_track(s.lib, track);
  if (t == NULL)
    return;
  if (s.art_track == track && (s.has_art || s.art_req != 0))
    return; // already have (or are fetching) this song's art
  art_release();
  s.art_track = track;
  s.art_req = ipod_art_request(ipod_lib_str(s.lib, t->path),
                               t->picture_offset,
                               t->picture_size,
                               (uint16_t)s.art_box,
                               (uint16_t)s.refl_h,
                               BG_RGB565);
}

static void art_poll(void) {
  if (s.art_req == 0)
    return;
  ipod_art_image_t img;
  if (!ipod_art_take(s.art_req, &img))
    return;
  art_release();
  s.art = img;
  s.art_req = 0;
  s.has_art = img.pixels != NULL;
  if (s.has_art) {
    image_dsc_init(&s.art_dsc, s.art.pixels, s.art.w, s.art.h);
    if (s.art.reflection != NULL)
      image_dsc_init(&s.refl_dsc, s.art.reflection, s.art.w, s.art.refl_h);
  }
  np_show_art();
}

static lv_obj_t *mk_bar(lv_obj_t *parent, int32_t width, lv_obj_t **out_fill) {
  lv_obj_t *bar = plain_obj(parent);
  lv_obj_set_size(bar, width, 10);
  set_vgrad(bar, C_BAR_TOP, C_BAR_BOT);
  lv_obj_set_style_border_width(bar, 1, 0);
  lv_obj_set_style_border_color(bar, lv_color_hex(C_BAR_LINE), 0);
  lv_obj_set_style_radius(bar, 5, 0);
  lv_obj_set_style_clip_corner(bar, true, 0);
  lv_obj_t *fill = plain_obj(bar);
  lv_obj_set_height(fill, lv_pct(100));
  lv_obj_set_width(fill, 0);
  set_vgrad(fill, C_FILL_TOP, C_FILL_BOT);
  *out_fill = fill;
  return bar;
}

static void np_create(lv_obj_t *page) {
  const int32_t w = s.w;
  int32_t text_x;
  int32_t text_w;
  int32_t text_y;
  int32_t bar_y;
  lv_obj_t *art_box = plain_obj(page);
  if (!s.is_landscape) {
    s.art_box = w * 57 / 100; // 136 px on the 240 px wide panel
    s.refl_h = s.art_box / 5;
    lv_obj_set_size(art_box, s.art_box, s.art_box + s.refl_h);
    lv_obj_align(art_box, LV_ALIGN_TOP_MID, 0, 20);
    text_x = 8;
    text_w = w - 16;
    text_y = 20 + s.art_box + s.refl_h + 2;
    bar_y = s.body_h - 36;
  } else {
    s.art_box = s.body_h - 74;
    s.refl_h = s.art_box / 6;
    lv_obj_set_size(art_box, s.art_box, s.art_box + s.refl_h);
    lv_obj_set_pos(art_box, 12, 12);
    text_x = s.art_box + 24;
    text_w = w - text_x - 8;
    text_y = 30;
    bar_y = s.body_h - 36;
  }

  s.np_art_ph = plain_obj(art_box);
  lv_obj_set_size(s.np_art_ph, s.art_box, s.art_box);
  set_vgrad(s.np_art_ph, C_ART_PH_TOP, C_ART_PH_BOT);
  lv_obj_set_style_border_width(s.np_art_ph, 1, 0);
  lv_obj_set_style_border_color(s.np_art_ph, lv_color_hex(C_ART_PH_BOT), 0);
  lv_obj_t *note = mk_label(s.np_art_ph, &lv_font_montserrat_16, C_SEL_TEXT);
  lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lv_obj_center(note);
  lv_obj_set_style_transform_scale(note, LETTER_SCALE, 0);
  lv_obj_set_style_transform_pivot_x(note, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(note, lv_pct(50), 0);
  s.np_art = lv_image_create(art_box);
  s.np_refl = lv_image_create(art_box);
  set_hidden(s.np_art, true);
  set_hidden(s.np_refl, true);

  s.np_counter = mk_label(page, &lv_font_montserrat_12, C_SUBTEXT);
  if (s.is_landscape)
    lv_obj_set_pos(s.np_counter, text_x, 10);
  else
    lv_obj_set_pos(s.np_counter, 8, 4);

  s.np_title = mk_label(page, &lv_font_montserrat_16, C_TEXT);
  s.np_artist = mk_label(page, &lv_font_montserrat_14, C_TEXT);
  s.np_album = mk_label(page, &lv_font_montserrat_14, C_SUBTEXT);
  lv_obj_t *lines[3] = {s.np_title, s.np_artist, s.np_album};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_width(lines[i], text_w);
    lv_label_set_long_mode(lines[i], LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(
        lines[i], s.is_landscape ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lines[i], text_x, text_y + i * 20 + (i > 0 ? 2 : 0));
  }

  // Progress bar with elapsed / remaining time (and the scrubber diamond).
  s.np_bar_w = w - 40;
  s.np_bar_y = bar_y;
  s.np_bar = mk_bar(page, s.np_bar_w, &s.np_fill);
  lv_obj_set_pos(s.np_bar, 20, bar_y);
  s.np_knob = plain_obj(page);
  lv_obj_set_size(s.np_knob, 10, 10);
  lv_obj_set_style_bg_color(s.np_knob, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s.np_knob, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s.np_knob, 2, 0);
  lv_obj_set_style_border_color(s.np_knob, lv_color_hex(C_FILL_BOT), 0);
  lv_obj_set_style_transform_rotation(s.np_knob, 450, 0);
  lv_obj_set_style_transform_pivot_x(s.np_knob, 5, 0);
  lv_obj_set_style_transform_pivot_y(s.np_knob, 5, 0);
  set_hidden(s.np_knob, true);
  s.np_elapsed = mk_label(page, &lv_font_montserrat_12, C_TEXT);
  lv_obj_set_pos(s.np_elapsed, 20, bar_y + 14);
  s.np_remain = mk_label(page, &lv_font_montserrat_12, C_TEXT);
  lv_obj_align(s.np_remain, LV_ALIGN_TOP_RIGHT, -20, bar_y + 14);

  // Volume bar (speaker icons at both ends), shown while the volume changes.
  s.np_vol = plain_obj(page);
  lv_obj_set_size(s.np_vol, w - 16, 30);
  lv_obj_set_pos(s.np_vol, 8, bar_y - 8);
  lv_obj_set_style_bg_color(s.np_vol, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(s.np_vol, LV_OPA_COVER, 0);
  lv_obj_t *vmin = mk_label(s.np_vol, &lv_font_montserrat_12, C_ARROW);
  lv_label_set_text(vmin, LV_SYMBOL_VOLUME_MID);
  lv_obj_align(vmin, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *vmax = mk_label(s.np_vol, &lv_font_montserrat_12, C_ARROW);
  lv_label_set_text(vmax, LV_SYMBOL_VOLUME_MAX);
  lv_obj_align(vmax, LV_ALIGN_RIGHT_MID, 0, 0);
  s.np_vol_bar_w = w - 70;
  lv_obj_t *vbar = mk_bar(s.np_vol, s.np_vol_bar_w, &s.np_vol_fill);
  lv_obj_align(vbar, LV_ALIGN_CENTER, 0, 0);
  set_hidden(s.np_vol, true);

  // Rating stars.
  const int32_t stars_w = RATING_STARS * STAR_PX + (RATING_STARS - 1) * STAR_GAP;
  for (int i = 0; i < RATING_STARS; i++) {
    s.np_stars[i] = lv_image_create(page);
    lv_image_set_src(s.np_stars[i], &s_star_empty_dsc);
    lv_obj_set_pos(s.np_stars[i], (w - stars_w) / 2 + i * (STAR_PX + STAR_GAP), bar_y - 4);
    set_hidden(s.np_stars[i], true);
  }

  // Shuffle mode label.
  s.np_mode_label = mk_label(page, &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_width(s.np_mode_label, w - 16);
  lv_obj_set_style_text_align(s.np_mode_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s.np_mode_label, 8, bar_y - 4);
  set_hidden(s.np_mode_label, true);

  s.np_track_shown = IPOD_NO_TRACK;
  s.np_seq_shown = 0;
  s.wheel_mode = WHEEL_VOLUME;
  np_show_art();
  np_update(lv_tick_get());
}

static void np_update(uint32_t now) {
  if (s.np_title == NULL)
    return;
  const ipod_player_status_t *st = &s.st;
  if (st->has_track && (st->track != s.np_track_shown || st->track_seq != s.np_seq_shown)) {
    const ipod_track_t *t = ipod_lib_track(s.lib, st->track);
    if (t != NULL) {
      set_label_if_changed(s.np_title, ipod_lib_disp(s.lib, t->title));
      set_label_if_changed(s.np_artist, ipod_lib_disp(s.lib, t->artist));
      set_label_if_changed(s.np_album, ipod_lib_disp(s.lib, t->album));
      art_request_for(st->track);
      np_show_art();
    }
    s.np_track_shown = st->track;
    s.np_seq_shown = st->track_seq;
  }
  char buf[32];
  if (st->queue_count > 0)
    snprintf(
        buf, sizeof(buf), "%u of %u", (unsigned)(st->queue_pos + 1), (unsigned)st->queue_count);
  else
    buf[0] = '\0';
  set_label_if_changed(s.np_counter, buf);

  if (s.wheel_mode != WHEEL_VOLUME && (int32_t)(now - s.mode_until) >= 0)
    s.wheel_mode = WHEEL_VOLUME;
  const bool show_volume = s.wheel_mode == WHEEL_VOLUME && (int32_t)(now - s.volume_until) < 0;
  const bool show_rating = s.wheel_mode == WHEEL_RATING;
  const bool show_shuffle = s.wheel_mode == WHEEL_SHUFFLE;
  const bool show_progress = !show_volume && !show_rating && !show_shuffle;

  set_hidden(s.np_bar, !show_progress);
  set_hidden(s.np_elapsed, !show_progress);
  set_hidden(s.np_remain, !show_progress);
  set_hidden(s.np_knob, !(show_progress && s.wheel_mode == WHEEL_SCRUB));
  set_hidden(s.np_vol, !show_volume);
  for (int i = 0; i < RATING_STARS; i++)
    set_hidden(s.np_stars[i], !show_rating);
  set_hidden(s.np_mode_label, !show_shuffle);

  if (show_progress) {
    const uint32_t dur = st->dur_ms;
    const uint32_t pos = st->pos_ms < dur ? st->pos_ms : dur;
    const int32_t fill = dur > 0 ? (int32_t)((int64_t)(s.np_bar_w - 2) * pos / dur) : 0;
    lv_obj_set_width(s.np_fill, fill);
    ipod_text_fmt_time(buf, sizeof(buf), pos);
    set_label_if_changed(s.np_elapsed, buf);
    char rem[24];
    ipod_text_fmt_time(rem, sizeof(rem), dur - pos);
    snprintf(buf, sizeof(buf), "-%s", rem);
    set_label_if_changed(s.np_remain, buf);
    lv_obj_align(s.np_remain, LV_ALIGN_TOP_RIGHT, -20, s.np_bar_y + 14);
    if (s.wheel_mode == WHEEL_SCRUB)
      lv_obj_set_pos(s.np_knob, 20 + fill - 4, s.np_bar_y);
  }
  if (show_volume) {
    lv_obj_set_width(s.np_vol_fill, (s.np_vol_bar_w - 2) * s.settings.volume / IPOD_VOLUME_MAX);
  }
  if (show_rating) {
    const ipod_track_t *t = ipod_lib_track(s.lib, st->track);
    const int rating = t != NULL ? t->rating : 0;
    for (int i = 0; i < RATING_STARS; i++)
      lv_image_set_src(s.np_stars[i], i < rating ? &s_star_full_dsc : &s_star_empty_dsc);
  }
  if (show_shuffle) {
    snprintf(
        buf, sizeof(buf), LV_SYMBOL_SHUFFLE "  Shuffle: %s", ipod_shuffle_name(s.settings.shuffle));
    set_label_if_changed(s.np_mode_label, buf);
  }
}

// ---------------------------------------------------------------------------
// Search page

static void search_run(void) {
  free(s.hits);
  s.hits = NULL;
  s.nhits = ipod_lib_search(s.lib, s.query, SEARCH_HITS_MAX, &s.hits);
  s.hit_sel = 0;
  s.hit_top = 0;
}

static void search_create(lv_obj_t *page) {
  const int32_t kb_h = 64;
  rows_create(page, 0, s.body_h - kb_h - 4, s.w);
  lv_obj_t *kb = plain_obj(page);
  lv_obj_set_size(kb, s.w, kb_h);
  lv_obj_set_pos(kb, 0, s.body_h - kb_h);
  set_vgrad(kb, C_HEADER_TOP, C_HEADER_BOT);
  lv_obj_set_style_border_side(kb, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(kb, 1, 0);
  lv_obj_set_style_border_color(kb, lv_color_hex(C_HEADER_LINE), 0);

  lv_obj_t *field = plain_obj(kb);
  lv_obj_set_size(field, s.w - 70, 22);
  lv_obj_set_pos(field, 8, 5);
  lv_obj_set_style_bg_color(field, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(field, 1, 0);
  lv_obj_set_style_border_color(field, lv_color_hex(C_BAR_LINE), 0);
  lv_obj_set_style_radius(field, 11, 0);
  s.sr_query = mk_label(field, &lv_font_montserrat_14, C_TEXT);
  lv_obj_set_width(s.sr_query, s.w - 90);
  lv_label_set_long_mode(s.sr_query, LV_LABEL_LONG_MODE_CLIP);
  lv_obj_align(s.sr_query, LV_ALIGN_LEFT_MID, 10, 0);
  s.sr_count = mk_label(kb, &lv_font_montserrat_12, C_SUBTEXT);
  lv_obj_align(s.sr_count, LV_ALIGN_TOP_RIGHT, -8, 9);

  const int32_t strip_x = (s.w - KEYS_VISIBLE * KEY_W) / 2;
  for (int i = 0; i < KEYS_VISIBLE; i++) {
    lv_obj_t *cell = plain_obj(kb);
    lv_obj_set_size(cell, KEY_W, 26);
    lv_obj_set_pos(cell, strip_x + i * KEY_W, 33);
    lv_obj_set_style_radius(cell, 4, 0);
    set_vgrad(cell, C_SEL_TOP, C_SEL_BOT);
    lv_obj_set_style_bg_opa(cell, i == KEYS_VISIBLE / 2 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_t *l = mk_label(cell, &lv_font_montserrat_14, C_TEXT);
    lv_obj_center(l);
    s.sr_keys[i] = l;
  }
  search_render();
}

static void search_render(void) {
  if (s.sr_query == NULL)
    return;
  char q[SEARCH_MAX + 4];
  snprintf(q, sizeof(q), "%s%s", s.query, s.is_results_focus ? "" : "|");
  set_label_if_changed(s.sr_query, q);
  char count[24];
  snprintf(count, sizeof(count), "%u", (unsigned)s.nhits);
  set_label_if_changed(s.sr_count, s.query[0] != '\0' ? count : "");
  for (int i = 0; i < KEYS_VISIBLE; i++) {
    const int k = ((s.key_idx + i - KEYS_VISIBLE / 2) % SEARCH_KEY_COUNT + SEARCH_KEY_COUNT) %
                  SEARCH_KEY_COUNT;
    const char c = SEARCH_KEYS[k];
    char txt[8];
    if (c == SEARCH_KEY_DEL)
      ipod_text_copy(txt, sizeof(txt), LV_SYMBOL_BACKSPACE);
    else if (c == ' ')
      ipod_text_copy(txt, sizeof(txt), "_");
    else
      snprintf(txt, sizeof(txt), "%c", c);
    set_label_if_changed(s.sr_keys[i], txt);
    const bool centre = i == KEYS_VISIBLE / 2;
    lv_obj_set_style_text_color(
        s.sr_keys[i],
        lv_color_hex(centre && !s.is_results_focus ? C_SEL_TEXT : (centre ? C_BLUE : C_TEXT)),
        0);
    lv_obj_set_style_bg_opa(
        lv_obj_get_parent(s.sr_keys[i]), centre && !s.is_results_focus ? LV_OPA_COVER : 0, 0);
  }
  const int32_t width = s.row_w;
  if (s.hit_sel < s.hit_top)
    s.hit_top = s.hit_sel;
  if (s.hit_sel >= s.hit_top + s.vis_rows)
    s.hit_top = s.hit_sel - s.vis_rows + 1;
  for (int i = 0; i < s.vis_rows; i++) {
    const size_t idx = (size_t)(s.hit_top + i);
    if (idx >= s.nhits) {
      row_set(&s.rows[i], false, "", "", IPOD_ICON_NONE, false, width);
      continue;
    }
    const ipod_hit_t *h = &s.hits[idx];
    const ipod_track_t *t = ipod_lib_track(s.lib, h->track);
    const char *text = "";
    const char *kind = "";
    if (t != NULL) {
      switch (h->kind) {
        case IPOD_HIT_ARTIST:
          text = ipod_lib_disp(s.lib, t->album_artist);
          kind = "Artist";
          break;
        case IPOD_HIT_ALBUM:
          text = ipod_lib_disp(s.lib, t->album);
          kind = "Album";
          break;
        default:
          text = ipod_lib_disp(s.lib, t->title);
          kind = "Song";
          break;
      }
    }
    row_set(&s.rows[i],
            true,
            text,
            kind,
            IPOD_ICON_NONE,
            s.is_results_focus && (int)idx == s.hit_sel,
            width);
  }
}

// ---------------------------------------------------------------------------
// Extras: Clock and Stopwatch

static lv_obj_t *mk_big_label(lv_obj_t *page, int32_t scale, int32_t y) {
  lv_obj_t *l = mk_label(page, &lv_font_montserrat_16, C_TEXT);
  lv_obj_set_style_transform_scale(l, scale, 0);
  lv_obj_set_style_transform_pivot_x(l, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(l, lv_pct(50), 0);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

static void clock_update(void) {
  if (s.clk_time == NULL)
    return;
  bool synced = false;
  time_t now = ipod_port_time_now(&synced);
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  char buf[48];
  strftime(buf, sizeof(buf), "%H:%M", &tm_now);
  set_label_if_changed(s.clk_time, buf);
  lv_obj_align(s.clk_time, LV_ALIGN_TOP_MID, 0, s.body_h / 3);
  strftime(buf, sizeof(buf), "%A, %d %B %Y", &tm_now);
  if (!synced)
    snprintf(buf, sizeof(buf), "Time not set");
  set_label_if_changed(s.clk_date, buf);
}

static void clock_create(lv_obj_t *page) {
  s.clk_time = mk_big_label(page, CLOCK_SCALE, s.body_h / 3);
  s.clk_date = mk_label(page, &lv_font_montserrat_14, C_SUBTEXT);
  lv_obj_align(s.clk_date, LV_ALIGN_TOP_MID, 0, s.body_h / 3 + 50);
  lv_obj_t *tz = mk_label(page, &lv_font_montserrat_12, C_SUBTEXT);
  lv_label_set_text(tz, "UTC");
  lv_obj_align(tz, LV_ALIGN_TOP_MID, 0, s.body_h / 3 + 72);
  clock_update();
}

static uint32_t sw_elapsed(void) {
  return s.sw_accum_ms + (s.is_sw_running ? lv_tick_get() - s.sw_start_ms : 0);
}

static void fmt_stopwatch(char *buf, size_t n, uint32_t ms) {
  snprintf(buf,
           n,
           "%02u:%02u:%02u.%u",
           (unsigned)(ms / 3600000u),
           (unsigned)(ms / 60000u % 60u),
           (unsigned)(ms / 1000u % 60u),
           (unsigned)(ms / 100u % 10u));
}

static void stopwatch_update(void) {
  if (s.sw_time == NULL)
    return;
  char buf[32];
  fmt_stopwatch(buf, sizeof(buf), sw_elapsed());
  set_label_if_changed(s.sw_time, buf);
  lv_obj_align(s.sw_time, LV_ALIGN_TOP_MID, 0, 34);
  for (int i = 0; i < STOPWATCH_LAPS; i++) {
    if (i < s.sw_nlaps) {
      char t[24];
      fmt_stopwatch(t, sizeof(t), s.sw_lap_ms[i]);
      snprintf(buf, sizeof(buf), "Lap %d   %s", s.sw_nlaps - i, t);
    } else {
      buf[0] = '\0';
    }
    set_label_if_changed(s.sw_laps[i], buf);
  }
}

static void stopwatch_create(lv_obj_t *page) {
  s.sw_time = mk_big_label(page, STOPWATCH_SCALE, 34);
  for (int i = 0; i < STOPWATCH_LAPS; i++) {
    s.sw_laps[i] = mk_label(page, &lv_font_montserrat_14, C_TEXT);
    lv_obj_align(s.sw_laps[i], LV_ALIGN_TOP_MID, 0, 90 + i * 22);
  }
  lv_obj_t *hint = mk_label(page, &lv_font_montserrat_12, C_SUBTEXT);
  lv_label_set_text(hint, "OK start / stop    DOWN lap / reset");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);
  stopwatch_update();
}

static void stopwatch_toggle(void) {
  if (s.is_sw_running) {
    s.sw_accum_ms = sw_elapsed();
    s.is_sw_running = false;
  } else {
    s.sw_start_ms = lv_tick_get();
    s.is_sw_running = true;
  }
}

static void stopwatch_lap_or_reset(void) {
  if (s.is_sw_running) {
    memmove(&s.sw_lap_ms[1], &s.sw_lap_ms[0], (STOPWATCH_LAPS - 1) * sizeof(uint32_t));
    s.sw_lap_ms[0] = sw_elapsed();
    if (s.sw_nlaps < STOPWATCH_LAPS)
      s.sw_nlaps++;
  } else {
    s.sw_accum_ms = 0;
    s.sw_nlaps = 0;
  }
}

// ---------------------------------------------------------------------------
// Library update page

static void scan_page_create(lv_obj_t *page) {
  lv_obj_t *icon = mk_big_label(page, LETTER_SCALE, s.body_h / 4);
  lv_label_set_text(icon, LV_SYMBOL_REFRESH);
  lv_obj_t *msg = mk_label(page, &lv_font_montserrat_16, C_TEXT);
  lv_label_set_text(msg, "Updating Library...");
  lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, s.body_h / 2);
  s.scan_count = mk_label(page, &lv_font_montserrat_14, C_SUBTEXT);
  lv_obj_align(s.scan_count, LV_ALIGN_TOP_MID, 0, s.body_h / 2 + 28);
  lv_obj_t *hint = mk_label(page, &lv_font_montserrat_12, C_SUBTEXT);
  lv_label_set_text(hint, "Looking for FLAC files on the microSD card");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

// ---------------------------------------------------------------------------
// Pages and transitions

static void anim_x_cb(void *obj, int32_t v) {
  lv_obj_set_x(obj, v);
}

static void anim_done_cb(lv_anim_t *a) {
  lv_obj_t *old = a->var;
  if (old == s.old_page)
    s.old_page = NULL;
  lv_obj_delete(old);
}

static void finish_transition(void) {
  if (s.old_page != NULL) {
    lv_anim_delete(s.old_page, anim_x_cb);
    lv_obj_delete(s.old_page);
    s.old_page = NULL;
  }
  if (s.page != NULL) {
    lv_anim_delete(s.page, anim_x_cb);
    lv_obj_set_x(s.page, 0);
  }
}

static void slide(lv_obj_t *obj, int32_t from, int32_t to, bool delete_after) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, anim_x_cb);
  lv_anim_set_values(&a, from, to);
  lv_anim_set_duration(&a, ANIM_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  if (delete_after)
    lv_anim_set_completed_cb(&a, anim_done_cb);
  lv_anim_start(&a);
}

static void clear_page_refs(void) {
  memset(s.rows, 0, sizeof(s.rows));
  s.vis_rows = 0;
  s.row_w = 0;
  s.sb_track = NULL;
  s.sb_thumb = NULL;
  s.np_counter = NULL;
  s.np_art = NULL;
  s.np_art_ph = NULL;
  s.np_refl = NULL;
  s.np_title = NULL;
  s.np_artist = NULL;
  s.np_album = NULL;
  s.np_bar = NULL;
  s.np_fill = NULL;
  s.np_knob = NULL;
  s.np_elapsed = NULL;
  s.np_remain = NULL;
  s.np_vol = NULL;
  s.np_vol_fill = NULL;
  memset(s.np_stars, 0, sizeof(s.np_stars));
  s.np_mode_label = NULL;
  s.sr_query = NULL;
  s.sr_count = NULL;
  memset(s.sr_keys, 0, sizeof(s.sr_keys));
  s.clk_time = NULL;
  s.clk_date = NULL;
  s.sw_time = NULL;
  memset(s.sw_laps, 0, sizeof(s.sw_laps));
  s.scan_count = NULL;
}

/** @brief Rebuild the page for the top view. dir: 1 = forward, -1 = back, 0 = in place. */
static void build_page(int dir) {
  finish_transition();
  lv_obj_t *old = s.page;
  clear_page_refs();

  lv_obj_t *page = plain_obj(s_screen);
  lv_obj_set_size(page, s.w, s.body_h);
  lv_obj_set_pos(page, 0, HEADER_H);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  s.page = page;

  const ipod_view_t *v = top_view();
  if (s.is_scan_modal) {
    scan_page_create(page);
  } else if (v != NULL) {
    switch (v->kind) {
      case IPOD_VIEW_NOW_PLAYING:
        np_create(page);
        break;
      case IPOD_VIEW_SEARCH:
        search_create(page);
        break;
      case IPOD_VIEW_CLOCK:
        clock_create(page);
        break;
      case IPOD_VIEW_STOPWATCH:
        stopwatch_create(page);
        break;
      case IPOD_VIEW_MAIN:
        // "Now Playing" comes and goes with playback: rebuild on every visit.
        s.menu.has_now_playing = s.st.has_track;
        ipod_menu_refresh(&s.menu);
        list_create(page);
        break;
      default:
        list_create(page);
        break;
    }
  }
  lv_obj_move_foreground(s.letter_box);
  lv_obj_move_foreground(s.toast);
  set_hidden(s.letter_box, true);
  header_update();

  if (old != NULL) {
    if (dir == 0) {
      lv_obj_delete(old);
    } else {
      s.old_page = old;
      slide(page, dir * s.w, 0, false);
      slide(old, 0, -dir * s.w, true);
    }
  }
}

// ---------------------------------------------------------------------------
// Library scanning (background or modal "Updating Library")

static void scan_task(void *arg) {
  scan_ctx_t *ctx = arg;
  ctx->result = ipod_lib_scan(ctx->root, ctx->prev, &ctx->prog);
  xSemaphoreGive(ctx->exited); // publishes ctx->result to the UI thread
  vTaskDelete(NULL);
}

static void scan_ctx_free(scan_ctx_t *ctx) {
  if (ctx == NULL)
    return;
  ipod_lib_destroy(ctx->result);
  if (ctx->exited != NULL)
    vSemaphoreDelete(ctx->exited);
  free(ctx);
}

static void start_scan(bool is_modal) {
  if (s.scan != NULL) {
    if (is_modal && !s.is_scan_modal) {
      s.is_scan_modal = true; // a background scan is already running: just show it
      build_page(0);
    }
    return;
  }
  scan_ctx_t *ctx = calloc(1, sizeof(*ctx));
  if (ctx != NULL)
    ctx->exited = xSemaphoreCreateBinary();
  if (ctx == NULL || ctx->exited == NULL) {
    ESP_LOGE(TAG, "scan alloc failed");
    scan_ctx_free(ctx);
    return;
  }
  struct stat st;
  ipod_text_copy(ctx->root,
                 sizeof(ctx->root),
                 (stat(MUSIC_DIR, &st) == 0 && S_ISDIR(st.st_mode)) ? MUSIC_DIR : VFS_MOUNT_POINT);
  ctx->prog.skip_stats = true; // the player may update play counts meanwhile
  ctx->prev = s.lib;
  if (xTaskCreatePinnedToCore(scan_task,
                              "ipod_scan",
                              SCAN_TASK_STACK,
                              ctx,
                              SYS_PRIO_BACKGROUND,
                              NULL,
                              SYS_CORE_RADIO) != pdPASS) {
    ESP_LOGE(TAG, "scan task create failed");
    scan_ctx_free(ctx);
    return;
  }
  s.scan = ctx;
  s.is_scan_modal = is_modal;
  if (is_modal)
    build_page(0);
}

// Swap in a freshly scanned library: playback stops, the menus restart.
static void apply_library(ipod_library_t *fresh) {
  ipod_library_t *old = s.lib;
  ipod_player_set_library(fresh); // the player lets go of the old library
  ipod_lib_copy_stats(fresh, old);
  art_release();
  s.art_req = 0;
  s.art_track = IPOD_NO_TRACK;
  s.lib = fresh;
  ipod_menu_set_library(&s.menu, fresh);
  ipod_lib_destroy(old);
  load_otg();
  s.is_cache_dirty = true;
  save_cache();
  ESP_LOGI(TAG, "library updated: %u songs", (unsigned)ipod_lib_count(s.lib));
}

static void scan_poll(void) {
  if (s.scan == NULL) {
    // A background result waits until nothing is playing and we are home.
    if (s.pending_lib != NULL && s.st.state == IPOD_PLAYER_STOPPED && !s.st.has_track &&
        s.menu.depth <= 1) {
      ipod_library_t *fresh = s.pending_lib;
      s.pending_lib = NULL;
      apply_library(fresh);
      build_page(0);
    }
    return;
  }
  if (s.scan_count != NULL) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u songs found", (unsigned)atomic_load(&s.scan->prog.files_found));
    set_label_if_changed(s.scan_count, buf);
  }
  if (xSemaphoreTake(s.scan->exited, 0) != pdTRUE)
    return;
  ipod_library_t *fresh = s.scan->result;
  s.scan->result = NULL;
  scan_ctx_free(s.scan);
  s.scan = NULL;
  const bool was_modal = s.is_scan_modal;
  s.is_scan_modal = false;
  if (fresh == NULL) {
    if (was_modal)
      build_page(0);
    return;
  }
  if (was_modal) {
    apply_library(fresh);
    build_page(0);
    show_toast(ipod_lib_count(s.lib) > 0 ? "Library updated" : "No FLAC files found");
  } else if (ipod_lib_same_files(fresh, s.lib)) {
    ipod_lib_destroy(fresh); // nothing changed on the card
  } else {
    ipod_lib_destroy(s.pending_lib);
    s.pending_lib = fresh;
  }
}

// ---------------------------------------------------------------------------
// Actions

static void play_tracks(const uint32_t *tracks, size_t count, size_t start) {
  uint32_t *groups = ipod_menu_album_groups(&s.menu, tracks, count);
  ipod_player_play_list(tracks, groups, count, start);
  free(groups);
  ipod_player_get_status(&s.st);
  s.menu.has_now_playing = true;
  ipod_menu_push(&s.menu, IPOD_VIEW_NOW_PLAYING, "Now Playing");
  build_page(1);
}

static void shuffle_all(void) {
  uint32_t *tracks = NULL;
  size_t n = ipod_lib_query_tracks(s.lib, NULL, IPOD_ORDER_TITLE, &tracks);
  if (n == 0) {
    show_toast("No songs");
    return;
  }
  s.settings.shuffle = IPOD_SHUFFLE_SONGS; // like the iPod, this turns shuffle on
  apply_settings();
  save_settings();
  play_tracks(tracks, n, SIZE_MAX);
  free(tracks);
}

static void menu_select(void) {
  ipod_menu_result_t r = ipod_menu_select(&s.menu);
  switch (r.event) {
    case IPOD_MENU_PUSHED: {
      const ipod_view_t *v = top_view();
      if (v != NULL && v->kind == IPOD_VIEW_SEARCH) {
        s.query[0] = '\0';
        s.key_idx = 0;
        s.is_results_focus = false;
        free(s.hits);
        s.hits = NULL;
        s.nhits = 0;
      }
      build_page(1);
      break;
    }
    case IPOD_MENU_PLAY:
      play_tracks(r.tracks, r.count, r.start);
      break;
    case IPOD_MENU_SHUFFLE_ALL:
      shuffle_all();
      break;
    case IPOD_MENU_SETTING: {
      apply_settings();
      save_settings();
      const ipod_view_t *v = top_view();
      if (v != NULL && v->kind == IPOD_VIEW_SETTINGS &&
          (r.setting == IPOD_SETTING_EQ || r.setting == IPOD_SETTING_VOLUME_LIMIT ||
           r.setting == IPOD_SETTING_ALL)) {
        build_page(-1); // came back from a choice list
        if (r.setting == IPOD_SETTING_ALL)
          show_toast("Settings reset");
      } else {
        list_render();
      }
      break;
    }
    case IPOD_MENU_UPDATE_LIBRARY:
      ipod_player_stop();
      start_scan(true);
      break;
    case IPOD_MENU_OTG_CHANGED:
      save_otg();
      build_page(0);
      break;
    case IPOD_MENU_POPPED:
      build_page(-1);
      break;
    default:
      break;
  }
}

static void search_select(void) {
  if (!s.is_results_focus) {
    const char c = SEARCH_KEYS[s.key_idx];
    const size_t len = strlen(s.query);
    if (c == SEARCH_KEY_DEL) {
      if (len > 0)
        s.query[len - 1] = '\0';
    } else if (len < SEARCH_MAX) {
      s.query[len] = c;
      s.query[len + 1] = '\0';
    }
    search_run();
    search_render();
    return;
  }
  if (s.hit_sel < 0 || (size_t)s.hit_sel >= s.nhits)
    return;
  const ipod_hit_t hit = s.hits[s.hit_sel];
  if (hit.kind == IPOD_HIT_SONG) {
    // Play the songs among the results, starting with the chosen one.
    uint32_t *songs = malloc(s.nhits * sizeof(uint32_t));
    if (songs == NULL)
      return;
    size_t n = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.nhits; i++) {
      if (s.hits[i].kind != IPOD_HIT_SONG)
        continue;
      if ((int)i == s.hit_sel)
        start = n;
      songs[n++] = s.hits[i].track;
    }
    play_tracks(songs, n, start);
    free(songs);
    return;
  }
  const ipod_field_t field = hit.kind == IPOD_HIT_ARTIST ? IPOD_FIELD_ARTIST : IPOD_FIELD_ALBUM;
  if (ipod_menu_open_group(&s.menu, field, hit.track))
    build_page(1);
}

static void select_action(void) {
  ipod_view_t *v = top_view();
  if (v == NULL)
    return;
  switch (v->kind) {
    case IPOD_VIEW_NOW_PLAYING:
      // The centre button cycles what the wheel controls, like the iPod.
      s.wheel_mode = (wheel_mode_t)((s.wheel_mode + 1) % WHEEL_MODE_COUNT);
      s.mode_until = lv_tick_get() + MODE_TIMEOUT_MS;
      s.volume_until = 0;
      np_update(lv_tick_get());
      break;
    case IPOD_VIEW_SEARCH:
      search_select();
      break;
    case IPOD_VIEW_STOPWATCH:
      stopwatch_toggle();
      stopwatch_update();
      break;
    case IPOD_VIEW_CLOCK:
      break;
    default:
      menu_select();
      break;
  }
}

static void hold_action(void) {
  ipod_view_t *v = top_view();
  if (v == NULL)
    return;
  size_t added = 0;
  if (v->kind == IPOD_VIEW_NOW_PLAYING) {
    if (s.st.has_track)
      added = ipod_menu_otg_add(&s.menu, &s.st.track, 1);
  } else if (v->kind == IPOD_VIEW_SEARCH) {
    if (s.is_results_focus && (size_t)s.hit_sel < s.nhits &&
        s.hits[s.hit_sel].kind == IPOD_HIT_SONG)
      added = ipod_menu_otg_add(&s.menu, &s.hits[s.hit_sel].track, 1);
  } else if (v->kind == IPOD_VIEW_STOPWATCH) {
    s.is_sw_running = false;
    s.sw_accum_ms = 0;
    s.sw_nlaps = 0;
    stopwatch_update();
    return;
  } else if (is_list_view(v)) {
    added = ipod_menu_hold_select(&s.menu);
  }
  if (added > 0) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Added to On-The-Go (%u)", (unsigned)s.menu.otg_count);
    show_toast(msg);
    save_otg();
  }
}

static void back_action(void) {
  ipod_view_t *v = top_view();
  if (v != NULL && v->kind == IPOD_VIEW_SEARCH && s.is_results_focus) {
    s.is_results_focus = false; // MENU from the results returns to the letters
    search_render();
    return;
  }
  if (ipod_menu_back(&s.menu)) {
    s.wheel_mode = WHEEL_VOLUME;
    build_page(-1);
    return;
  }
  leave_app();
}

static void leave_app(void) {
  // ui_switch_screen runs our close hook (ui_ipod_stop): nothing may touch the
  // app state after this call.
  ui_switch_screen(s_return);
}

// Holding UP/DOWN spins the wheel faster and faster: the step doubles every
// two repeats, capped so a long list is crossed in a few seconds and short
// lists still move one row at a time.
static int wheel_step(const ipod_view_t *v) {
  if (s.wheel_repeats < ACCEL_START_REPEATS)
    return 1;
  int shift = (s.wheel_repeats - ACCEL_START_REPEATS) / 2;
  if (shift > ACCEL_MAX_SHIFT)
    shift = ACCEL_MAX_SHIFT;
  int cap = v != NULL ? (int)(v->nrows / ACCEL_LIST_FRACTION) : 1;
  if (cap < 1)
    cap = 1;
  const int step = 1 << shift;
  return step < cap ? step : cap;
}

static void wheel(int dir) {
  ipod_view_t *v = top_view();
  if (v == NULL)
    return;
  const uint32_t now = lv_tick_get();
  // On a d-pad UP reads as "more": louder, more stars, scrub forward.
  const int inc = -dir;
  switch (v->kind) {
    case IPOD_VIEW_NOW_PLAYING:
      switch (s.wheel_mode) {
        case WHEEL_VOLUME: {
          int vol = (int)s.settings.volume + inc * VOLUME_STEP;
          if (vol < 0)
            vol = 0;
          if (vol > s.settings.volume_limit)
            vol = s.settings.volume_limit;
          s.settings.volume = (uint8_t)vol;
          ipod_player_set_volume(s.settings.volume);
          s.volume_until = now + VOLUME_SHOW_MS;
          break;
        }
        case WHEEL_SCRUB: {
          int32_t step = s.wheel_repeats < 8    ? SCRUB_STEP_1_MS
                         : s.wheel_repeats < 20 ? SCRUB_STEP_2_MS
                                                : SCRUB_STEP_3_MS;
          ipod_player_seek_by(inc * step);
          s.mode_until = now + MODE_TIMEOUT_MS;
          break;
        }
        case WHEEL_RATING: {
          ipod_track_t *t = ipod_lib_track_mut(s.lib, s.st.track);
          if (t != NULL && s.st.has_track) {
            int r = (int)t->rating + inc;
            if (r < 0)
              r = 0;
            if (r > IPOD_RATING_MAX)
              r = IPOD_RATING_MAX;
            t->rating = (uint8_t)r;
            s.is_cache_dirty = true;
          }
          s.mode_until = now + MODE_TIMEOUT_MS;
          break;
        }
        case WHEEL_SHUFFLE:
          s.settings.shuffle = (ipod_shuffle_t)((s.settings.shuffle + IPOD_SHUFFLE_COUNT + inc) %
                                                IPOD_SHUFFLE_COUNT);
          ipod_player_set_shuffle(s.settings.shuffle);
          save_settings();
          s.mode_until = now + MODE_TIMEOUT_MS;
          break;
        default:
          break;
      }
      click();
      ipod_player_get_status(&s.st);
      np_update(now);
      return;
    case IPOD_VIEW_SEARCH:
      // LEFT / RIGHT pick letters; UP / DOWN move between the strip and results.
      if (s.is_results_focus) {
        int sel = s.hit_sel + dir;
        if (sel < 0) {
          s.is_results_focus = false;
          click();
        } else if ((size_t)sel < s.nhits) {
          s.hit_sel = sel;
          click();
        }
      } else if (dir > 0 && s.nhits > 0 && s.wheel_repeats == 0) {
        s.is_results_focus = true;
        s.hit_sel = 0;
        click();
      }
      search_render();
      return;
    case IPOD_VIEW_STOPWATCH:
      if (dir > 0 && s.wheel_repeats == 0) {
        stopwatch_lap_or_reset();
        stopwatch_update();
      }
      return;
    case IPOD_VIEW_CLOCK:
      return;
    default:
      break;
  }
  const int old = v->sel;
  ipod_menu_move(&s.menu, dir * wheel_step(v), s.vis_rows);
  if (v->sel != old) {
    click();
    list_render();
  }
  if (s.wheel_repeats >= LETTER_MIN_REPEAT && v->has_letters && (int)v->nrows > 2 * s.vis_rows) {
    char letter = ipod_menu_row_letter(&s.menu, v, (size_t)v->sel);
    if (letter != 0)
      show_letter(letter);
  }
}

static void toggle_hold(void) {
  s.is_hold = !s.is_hold;
  header_update();
  show_toast(s.is_hold ? "Hold on" : "Hold off");
}

static int32_t seek_step(void) {
  if (s.seek_repeats < SEEK_REPEATS_1)
    return SEEK_STEP_1_MS;
  if (s.seek_repeats < SEEK_REPEATS_2)
    return SEEK_STEP_2_MS;
  return SEEK_STEP_3_MS;
}

static void ipod_input(const input_event_t *ev, void *ctx) {
  (void)ctx;
  const input_button_t b = ev->button;
  const input_action_t a = ev->action;
  const bool is_ok_back = b == INPUT_BTN_OK || b == INPUT_BTN_BACK;

  if (is_ok_back && (a == INPUT_ACTION_PRESS || a == INPUT_ACTION_RELEASE)) {
    bool *down = b == INPUT_BTN_OK ? &s.is_ok_down : &s.is_back_down;
    *down = a == INPUT_ACTION_PRESS;
  }
  // HOLD switch: OK + BACK held together.
  if (a == INPUT_ACTION_LONG_PRESS && is_ok_back && s.is_ok_down && s.is_back_down) {
    if (!s.is_chord) {
      s.is_chord = true;
      toggle_hold();
    }
    return;
  }
  if (s.is_chord && is_ok_back) {
    // Swallow everything until both buttons of the chord are released.
    if (!s.is_ok_down && !s.is_back_down)
      s.is_chord = false;
    return;
  }
  if (s.is_hold) {
    if (a == INPUT_ACTION_PRESS)
      show_toast("Hold is on");
    return;
  }
  if (s.is_scan_modal) {
    // Library update in progress: only MENU (cancel / leave) is live.
    if (b == INPUT_BTN_BACK && a == INPUT_ACTION_RELEASE) {
      if (ipod_lib_count(s.lib) > 0 && s.scan != NULL)
        atomic_store(&s.scan->prog.cancel, true);
      else
        leave_app();
    }
    return;
  }

  const ipod_view_t *v = top_view();
  const bool search_keys = v != NULL && v->kind == IPOD_VIEW_SEARCH && !s.is_results_focus;
  switch (b) {
    case INPUT_BTN_UP:
    case INPUT_BTN_DOWN: {
      const int dir = b == INPUT_BTN_DOWN ? 1 : -1;
      if (a == INPUT_ACTION_PRESS) {
        s.wheel_repeats = 0;
        wheel(dir);
      } else if (a == INPUT_ACTION_REPEAT) {
        s.wheel_repeats++;
        wheel(dir);
      }
      break;
    }
    case INPUT_BTN_LEFT:
    case INPUT_BTN_RIGHT: {
      const int dir = b == INPUT_BTN_RIGHT ? 1 : -1;
      if (search_keys) {
        if (a == INPUT_ACTION_PRESS || a == INPUT_ACTION_REPEAT) {
          s.key_idx = (s.key_idx + dir + SEARCH_KEY_COUNT) % SEARCH_KEY_COUNT;
          click();
          search_render();
        }
        break;
      }
      if (a == INPUT_ACTION_PRESS) {
        s.is_seeking = false;
        s.seek_repeats = 0;
      } else if (a == INPUT_ACTION_LONG_PRESS) {
        s.is_seeking = true; // hold: rewind / fast-forward
        ipod_player_seek_by(dir * SEEK_STEP_1_MS);
      } else if (a == INPUT_ACTION_REPEAT && s.is_seeking) {
        s.seek_repeats++;
        ipod_player_seek_by(dir * seek_step());
      } else if (a == INPUT_ACTION_RELEASE) {
        if (!s.is_seeking) {
          if (dir > 0)
            ipod_player_next();
          else
            ipod_player_prev();
        }
        s.is_seeking = false;
      }
      break;
    }
    case INPUT_BTN_OK:
      if (a == INPUT_ACTION_PRESS) {
        s.is_ok_long = false;
      } else if (a == INPUT_ACTION_LONG_PRESS) {
        s.is_ok_long = true;
        hold_action();
      } else if (a == INPUT_ACTION_RELEASE && !s.is_ok_long) {
        select_action();
      }
      break;
    case INPUT_BTN_BACK:
      if (a == INPUT_ACTION_PRESS) {
        s.is_back_long = false;
      } else if (a == INPUT_ACTION_LONG_PRESS) {
        s.is_back_long = true;
        ipod_player_toggle_pause(); // the wheel's PLAY/PAUSE
        ipod_player_get_status(&s.st);
        header_update();
      } else if (a == INPUT_ACTION_RELEASE && !s.is_back_long) {
        back_action();
      }
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Refresh

static void timer_cb(lv_timer_t *t) {
  (void)t;
  if (!s.is_open)
    return;
  const uint32_t now = lv_tick_get();
  ipod_player_get_status(&s.st);

  // "Now Playing" appears in the main menu once something is loaded.
  if (s.st.has_track != s.menu.has_now_playing) {
    s.menu.has_now_playing = s.st.has_track;
    const ipod_view_t *v = top_view();
    if (v != NULL && v->kind == IPOD_VIEW_MAIN) {
      ipod_menu_refresh(&s.menu);
      build_page(0);
    }
  }
  // When the list plays out, the iPod leaves Now Playing.
  if (s.st.end_seq != s.last_end_seq) {
    s.last_end_seq = s.st.end_seq;
    const ipod_view_t *v = top_view();
    if (v != NULL && v->kind == IPOD_VIEW_NOW_PLAYING && ipod_menu_back(&s.menu))
      build_page(-1);
  }
  if (ipod_player_take_stats_dirty())
    s.is_cache_dirty = true;

  header_update();
  art_poll();
  scan_poll();

  if (s.letter_box != NULL && (int32_t)(now - s.letter_until) >= 0) {
    set_hidden(s.letter_box, true);
    s.letter_shown = 0;
  }
  if (s.toast != NULL && (int32_t)(now - s.toast_until) >= 0)
    set_hidden(s.toast, true);

  const ipod_view_t *v = top_view();
  if (v == NULL || s.is_scan_modal)
    return;
  switch (v->kind) {
    case IPOD_VIEW_NOW_PLAYING:
      np_update(now);
      break;
    case IPOD_VIEW_CLOCK:
      clock_update();
      break;
    case IPOD_VIEW_STOPWATCH:
      stopwatch_update();
      break;
    case IPOD_VIEW_SEARCH:
      break;
    default:
      if (s.st.track_seq != s.list_track_seq)
        list_render(); // move the speaker icon to the new song
      break;
  }
}

// ---------------------------------------------------------------------------
// Public API

void ui_ipod_set_return(int screen) {
  s_return = (screen_id_t)screen;
}

void ui_ipod_open(void) {
  if (s.is_open)
    ui_ipod_stop();
  if (s_screen != NULL) {
    lv_obj_delete(s_screen);
    s_screen = NULL;
  }
  memset(&s, 0, sizeof(s));
  s.art_track = IPOD_NO_TRACK;

  static bool stars_ready;
  if (!stars_ready) {
    render_star(s_star_full, C_STAR, false);
    render_star(s_star_empty, C_STAR_EMPTY, true);
    image_dsc_init(&s_star_full_dsc, s_star_full, STAR_PX, STAR_PX);
    image_dsc_init(&s_star_empty_dsc, s_star_empty, STAR_PX, STAR_PX);
    stars_ready = true;
  }

  mkdir(IPOD_DIR, 0775);
  ipod_settings_defaults(&s.settings);
  ipod_settings_load(&s.settings, SETTINGS_PATH);

  s.lib = ipod_lib_load(CACHE_PATH);
  const bool has_cache = s.lib != NULL;
  if (!has_cache)
    s.lib = ipod_lib_create();
  if (s.lib == NULL) {
    ESP_LOGE(TAG, "out of memory");
    return;
  }
  ipod_menu_init(&s.menu, s.lib, &s.settings);
  load_otg();

  if (ipod_player_start(s.lib) != ESP_OK)
    ESP_LOGE(TAG, "player failed to start");
  if (ipod_art_start() != ESP_OK)
    ESP_LOGW(TAG, "album art disabled");
  apply_settings();
  ipod_player_get_status(&s.st);
  s.last_end_seq = s.st.end_seq;

  s.w = ui_screen_w();
  s.h = ui_screen_h();
  s.is_landscape = s.w > s.h;
  s.body_h = s.h - HEADER_H;
  s.is_open = true;

  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  s_screen = scr;
  header_create(scr);
  overlays_create(scr);

  // Show the cached library at once and refresh it in the background; with no
  // cache yet, the first scan is shown as "Updating Library".
  start_scan(!has_cache);
  if (has_cache)
    build_page(0);

  s.timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
  ui_input_set_screen_handler(ipod_input, NULL);
  ui_screen_load_owned(&s_screen, scr);
  ESP_LOGI(TAG, "opened with %u songs", (unsigned)ipod_lib_count(s.lib));
}

void ui_ipod_stop(void) {
  if (!s.is_open)
    return;
  s.is_open = false;
  bool scan_alive = false;
  if (s.scan != NULL) {
    atomic_store(&s.scan->prog.cancel, true);
    if (xSemaphoreTake(s.scan->exited, pdMS_TO_TICKS(SCAN_WAIT_MS)) == pdTRUE) {
      scan_ctx_free(s.scan);
    } else {
      // A stuck card read: abandon the task's context and the library it reads.
      ESP_LOGE(TAG, "library scan did not stop; leaking it");
      scan_alive = true;
    }
    s.scan = NULL;
  }
  ipod_player_shutdown();
  ipod_art_shutdown();
  if (ipod_player_take_stats_dirty())
    s.is_cache_dirty = true;

  if (s.pending_lib != NULL && !scan_alive) {
    // Keep the newer scan, with this session's play counts and ratings.
    ipod_lib_copy_stats(s.pending_lib, s.lib);
    ipod_lib_destroy(s.lib);
    s.lib = s.pending_lib;
    s.pending_lib = NULL;
    s.is_cache_dirty = true;
    ESP_LOGI(TAG, "saving the updated library for next time");
  }
  if (s.is_cache_dirty && ipod_lib_count(s.lib) > 0)
    save_cache();
  save_settings();

  if (s.timer != NULL)
    lv_timer_delete(s.timer);
  finish_transition();
  art_release();
  free(s.hits);
  ipod_menu_free(&s.menu);
  if (!scan_alive)
    ipod_lib_destroy(s.lib);
  // The screen object itself is deleted by the UI manager.
  memset(&s, 0, sizeof(s));
}

void ui_ipod_debug(ui_ipod_debug_t *out) {
  memset(out, 0, sizeof(*out));
  out->is_open = s.is_open;
  if (!s.is_open)
    return;
  const ipod_view_t *v = top_view();
  out->is_now_playing = v != NULL && v->kind == IPOD_VIEW_NOW_PLAYING;
  out->is_hold = s.is_hold;
  out->is_scanning = s.scan != NULL;
  if (s.title != NULL)
    ipod_text_copy(out->title, sizeof(out->title), lv_label_get_text(s.title));
  if (v != NULL && v->kind == IPOD_VIEW_SEARCH) {
    out->selected_index = s.is_results_focus ? s.hit_sel : s.key_idx;
    out->row_count = (int)s.nhits;
    if (s.is_results_focus && (size_t)s.hit_sel < s.nhits && s.rows[0].obj != NULL) {
      const ipod_track_t *t = ipod_lib_track(s.lib, s.hits[s.hit_sel].track);
      if (t != NULL)
        ipod_text_copy(out->selected,
                       sizeof(out->selected),
                       ipod_lib_disp(s.lib,
                                     s.hits[s.hit_sel].kind == IPOD_HIT_ARTIST  ? t->album_artist
                                     : s.hits[s.hit_sel].kind == IPOD_HIT_ALBUM ? t->album
                                                                                : t->title));
    } else {
      snprintf(
          out->selected, sizeof(out->selected), "key:%c query:%s", SEARCH_KEYS[s.key_idx], s.query);
    }
  } else if (v != NULL && is_list_view(v)) {
    out->selected_index = v->sel;
    out->row_count = (int)v->nrows;
    ipod_row_text_t rt;
    ipod_menu_row_text(&s.menu, v, (size_t)v->sel, IPOD_NO_TRACK, &rt);
    ipod_text_copy(out->selected, sizeof(out->selected), rt.text);
  }
  static const char *const MODES[] = {"volume", "scrub", "rating", "shuffle"};
  ipod_text_copy(out->wheel_mode, sizeof(out->wheel_mode), MODES[s.wheel_mode]);
  if (s.st.has_track) {
    const ipod_track_t *t = ipod_lib_track(s.lib, s.st.track);
    if (t != NULL) {
      ipod_text_copy(out->now_title, sizeof(out->now_title), ipod_lib_disp(s.lib, t->title));
      out->rating = t->rating;
    }
  }
  out->player_state = (int)s.st.state;
  out->pos_ms = s.st.pos_ms;
  out->dur_ms = s.st.dur_ms;
  out->queue_pos = s.st.queue_pos;
  out->queue_count = s.st.queue_count;
  out->volume = s.settings.volume;
  out->shuffle = (int)s.settings.shuffle;
  out->repeat = (int)s.settings.repeat;
  out->eq = s.settings.eq_preset;
  out->otg_count = s.menu.otg_count;
  out->library_count = ipod_lib_count(s.lib);
  out->is_art_shown = s.np_art != NULL && s.has_art;
  if (s.letter_shown != 0 && s.letter_box != NULL &&
      !lv_obj_has_flag(s.letter_box, LV_OBJ_FLAG_HIDDEN))
    snprintf(out->overlay, sizeof(out->overlay), "%c", s.letter_shown);
}
