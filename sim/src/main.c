// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator: runs the iPod app's unmodified sources on the desktop.
//
//   hb_sim [--sd DIR] [--scale N] [--launcher] [--headless] [--script FILE]
//
// Keys (same as TentacleOS's HLE): arrows / WASD = d-pad, Enter / Space = OK,
// Backspace / Esc = BACK, F12 = screenshot, Ctrl+Q = quit. The on-screen
// buttons can be clicked (and held) with the mouse.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>

#include "audio_i2s.h"
#include "hb_sim.h"
#include "hb_ui.h"
#include "lvgl.h"
#include "ui_ipod.h"
#include "ui_manager.h"

#define PANEL_SHORT    240
#define PANEL_LONG     320
#define DRAW_BUF_LINES 80
#define LOOP_SLEEP_US  4000
#define BEZEL          28
#define PAD_AREA_H     190
#define BTN_SIZE       44
#define SCRIPT_LINE    512
#define SCRIPT_MAX     4096
#define WAIT_POLL_MS   20

typedef struct {
  SDL_Rect r;
  input_button_t button;
} hit_t;

static int LCD_W = PANEL_SHORT; // portrait by default, like the High Boy
static int LCD_H = PANEL_LONG;
static uint16_t s_fb[PANEL_SHORT * PANEL_LONG];
static bool s_fb_dirty = true;
static SDL_Window *s_win;
static SDL_Renderer *s_ren;
static SDL_Texture *s_tex;
static int s_scale = 2;
static hit_t s_hits[INPUT_BTN_COUNT];
static bool s_mouse_btn_down[INPUT_BTN_COUNT];
static bool s_headless;
static int s_shot_counter;

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  const uint16_t *src = (const uint16_t *)px_map;
  const int32_t w = lv_area_get_width(area);
  for (int32_t y = area->y1; y <= area->y2; y++) {
    memcpy(&s_fb[y * LCD_W + area->x1], src, (size_t)w * sizeof(uint16_t));
    src += w;
  }
  s_fb_dirty = true;
  lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// PNG screenshots (stored deflate blocks: no zlib dependency)

static uint32_t crc_table[256];

static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
  uint8_t hdr[8];
  put_be32(hdr, len);
  memcpy(hdr + 4, type, 4);
  fwrite(hdr, 1, 8, f);
  if (len > 0)
    fwrite(data, 1, len, f);
  uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
  crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
  uint8_t c[4];
  put_be32(c, crc);
  fwrite(c, 1, 4, f);
}

static bool write_png(const char *path) {
  static bool crc_ready;
  if (!crc_ready) {
    crc_init();
    crc_ready = true;
  }
  const size_t row = 1 + LCD_W * 3;
  const size_t raw_len = row * LCD_H;
  uint8_t *raw = malloc(raw_len);
  const size_t nblocks = (raw_len + 65534) / 65535;
  const size_t z_len = 2 + raw_len + nblocks * 5 + 4;
  uint8_t *z = malloc(z_len);
  FILE *f = fopen(path, "wb");
  if (raw == NULL || z == NULL || f == NULL) {
    free(raw);
    free(z);
    if (f != NULL)
      fclose(f);
    return false;
  }
  for (int y = 0; y < LCD_H; y++) {
    uint8_t *p = raw + (size_t)y * row;
    *p++ = 0; // filter: none
    for (int x = 0; x < LCD_W; x++) {
      uint16_t c = s_fb[y * LCD_W + x];
      uint8_t r = (uint8_t)((c >> 11) & 0x1F);
      uint8_t g = (uint8_t)((c >> 5) & 0x3F);
      uint8_t b = (uint8_t)(c & 0x1F);
      *p++ = (uint8_t)((r << 3) | (r >> 2));
      *p++ = (uint8_t)((g << 2) | (g >> 4));
      *p++ = (uint8_t)((b << 3) | (b >> 2));
    }
  }
  size_t zi = 0;
  z[zi++] = 0x78;
  z[zi++] = 0x01;
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t off = 0; off < raw_len; off += 65535) {
    size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
    z[zi++] = (off + n == raw_len) ? 1 : 0;
    z[zi++] = (uint8_t)n;
    z[zi++] = (uint8_t)(n >> 8);
    z[zi++] = (uint8_t)~n;
    z[zi++] = (uint8_t)(~n >> 8);
    memcpy(z + zi, raw + off, n);
    zi += n;
  }
  for (size_t i = 0; i < raw_len; i++) {
    a = (a + raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  put_be32(z + zi, (b << 16) | a);
  zi += 4;

  static const uint8_t SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(SIG, 1, 8, f);
  uint8_t ihdr[13];
  put_be32(ihdr, LCD_W);
  put_be32(ihdr + 4, LCD_H);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // RGB
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  png_chunk(f, "IHDR", ihdr, sizeof(ihdr));
  png_chunk(f, "IDAT", z, (uint32_t)zi);
  png_chunk(f, "IEND", NULL, 0);
  bool ok = fclose(f) == 0;
  free(raw);
  free(z);
  return ok;
}

// ---------------------------------------------------------------------------
// Window: a stylised High Boy with clickable buttons

static void layout_hits(void) {
  const int screen_w = LCD_W * s_scale;
  const int top = BEZEL + LCD_H * s_scale + 30;
  const int cx = BEZEL + 70;
  const int cy = top + 70;
  const int d = BTN_SIZE;
  s_hits[INPUT_BTN_UP] = (hit_t){{cx - d / 2, cy - d - d / 2, d, d}, INPUT_BTN_UP};
  s_hits[INPUT_BTN_DOWN] = (hit_t){{cx - d / 2, cy + d / 2, d, d}, INPUT_BTN_DOWN};
  s_hits[INPUT_BTN_LEFT] = (hit_t){{cx - d - d / 2, cy - d / 2, d, d}, INPUT_BTN_LEFT};
  s_hits[INPUT_BTN_RIGHT] = (hit_t){{cx + d / 2, cy - d / 2, d, d}, INPUT_BTN_RIGHT};
  s_hits[INPUT_BTN_OK] =
      (hit_t){{BEZEL + screen_w - 2 * d - 30, cy - d / 2 + 18, d, d}, INPUT_BTN_OK};
  s_hits[INPUT_BTN_BACK] = (hit_t){{BEZEL + screen_w - d - 10, cy - d - 6, d, d}, INPUT_BTN_BACK};
}

static void render_window(void) {
  if (s_headless || s_ren == NULL)
    return;
  SDL_UpdateTexture(s_tex, NULL, s_fb, LCD_W * sizeof(uint16_t));
  SDL_SetRenderDrawColor(s_ren, 0x2A, 0x24, 0x38, 0xFF);
  SDL_RenderClear(s_ren);
  SDL_Rect frame = {BEZEL - 6, BEZEL - 6, LCD_W * s_scale + 12, LCD_H * s_scale + 12};
  SDL_SetRenderDrawColor(s_ren, 0x0E, 0x0B, 0x14, 0xFF);
  SDL_RenderFillRect(s_ren, &frame);
  SDL_Rect dst = {BEZEL, BEZEL, LCD_W * s_scale, LCD_H * s_scale};
  SDL_RenderCopy(s_ren, s_tex, NULL, &dst);
  for (int b = 0; b < INPUT_BTN_COUNT; b++) {
    const bool down = input_is_down((input_button_t)b);
    if (b == INPUT_BTN_OK || b == INPUT_BTN_BACK)
      SDL_SetRenderDrawColor(s_ren, down ? 0xFF : 0xB8, down ? 0xFF : 0x9A, 0xFF, 0xFF);
    else
      SDL_SetRenderDrawColor(
          s_ren, down ? 0xE0 : 0x6A, down ? 0xE0 : 0x62, down ? 0xF0 : 0x7A, 0xFF);
    SDL_RenderFillRect(s_ren, &s_hits[b].r);
  }
  SDL_RenderPresent(s_ren);
}

static void save_shot(const char *path) {
  char auto_name[64];
  if (path == NULL) {
    snprintf(auto_name, sizeof(auto_name), "hb_shot_%03d.png", s_shot_counter++);
    path = auto_name;
  }
  if (write_png(path))
    fprintf(stderr, "screenshot: %s\n", path);
  else
    fprintf(stderr, "screenshot FAILED: %s\n", path);
}

static input_button_t key_to_button(SDL_Keycode k) {
  switch (k) {
    case SDLK_UP:
    case SDLK_w:
      return INPUT_BTN_UP;
    case SDLK_DOWN:
    case SDLK_s:
      return INPUT_BTN_DOWN;
    case SDLK_LEFT:
    case SDLK_a:
      return INPUT_BTN_LEFT;
    case SDLK_RIGHT:
    case SDLK_d:
      return INPUT_BTN_RIGHT;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
      return INPUT_BTN_OK;
    case SDLK_BACKSPACE:
    case SDLK_ESCAPE:
      return INPUT_BTN_BACK;
    default:
      return INPUT_BTN_COUNT;
  }
}

static bool handle_sdl_events(void) {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        return false;
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q && (e.key.keysym.mod & KMOD_CTRL))
          return false;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F12) {
          save_shot(NULL);
          break;
        }
        if (e.key.repeat)
          break; // input_manager generates its own repeats
        input_button_t b = key_to_button(e.key.keysym.sym);
        if (b != INPUT_BTN_COUNT)
          hb_input_set(b, e.type == SDL_KEYDOWN, now_ms());
        break;
      }
      case SDL_MOUSEBUTTONDOWN: {
        SDL_Point p = {e.button.x, e.button.y};
        for (int b = 0; b < INPUT_BTN_COUNT; b++) {
          if (SDL_PointInRect(&p, &s_hits[b].r)) {
            s_mouse_btn_down[b] = true;
            hb_input_set((input_button_t)b, true, now_ms());
          }
        }
        break;
      }
      case SDL_MOUSEBUTTONUP:
        for (int b = 0; b < INPUT_BTN_COUNT; b++) {
          if (s_mouse_btn_down[b]) {
            s_mouse_btn_down[b] = false;
            hb_input_set((input_button_t)b, false, now_ms());
          }
        }
        break;
      default:
        break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Script engine (automated UI tests)

typedef struct {
  char lines[SCRIPT_MAX][SCRIPT_LINE];
  int count;
  int pc;
  uint32_t wait_until;
  int failures;
  // wait_until FIELD OP VALUE TIMEOUT
  bool is_polling;
  uint32_t poll_deadline;
  // find / select: 0 idle, 1 scrolling up to the top, 2 scrolling down
  int find_phase;
  int find_last_index;
  int find_steps;
  input_button_t find_release; ///< pending release, INPUT_BTN_COUNT if none
} script_t;

#define FIND_STEP_MS   40
#define FIND_MAX_STEPS 2000

static script_t *s_script;

static bool script_load(const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL) {
    fprintf(stderr, "cannot open script %s: %s\n", path, strerror(errno));
    return false;
  }
  s_script = calloc(1, sizeof(*s_script));
  if (s_script == NULL) {
    fclose(f);
    return false;
  }
  s_script->find_release = INPUT_BTN_COUNT;
  char line[SCRIPT_LINE];
  while (s_script->count < SCRIPT_MAX && fgets(line, sizeof(line), f) != NULL) {
    line[strcspn(line, "\r\n")] = '\0';
    int times = 1;
    const char *body = line;
    if (strncmp(line, "repeat ", 7) == 0) { // "repeat N command..." expands at load
      times = atoi(line + 7);
      body = strchr(line + 7, ' ');
      body = body != NULL ? body + 1 : "";
    }
    for (int i = 0; i < times && s_script->count < SCRIPT_MAX; i++)
      snprintf(s_script->lines[s_script->count++], SCRIPT_LINE, "%s", body);
  }
  fclose(f);
  return true;
}

static input_button_t parse_button(const char *s) {
  static const char *const NAMES[] = {"up", "down", "left", "right", "ok", "back"};
  for (int i = 0; i < INPUT_BTN_COUNT; i++) {
    if (strcmp(s, NAMES[i]) == 0)
      return (input_button_t)i;
  }
  return INPUT_BTN_COUNT;
}

// Resolve a state field to text for expect / wait_until.
static void field_value(const char *field, char *out, size_t len) {
  ui_ipod_debug_t d;
  ui_ipod_debug(&d);
  hb_audio_stats_t a;
  hb_audio_get_stats(&a);
  static const char *const STATES[] = {"stopped", "playing", "paused"};
  if (strcmp(field, "title") == 0)
    snprintf(out, len, "%s", d.title);
  else if (strcmp(field, "selected") == 0)
    snprintf(out, len, "%s", d.selected);
  else if (strcmp(field, "index") == 0)
    snprintf(out, len, "%d", d.selected_index);
  else if (strcmp(field, "rows") == 0)
    snprintf(out, len, "%d", d.row_count);
  else if (strcmp(field, "song") == 0)
    snprintf(out, len, "%s", d.now_title);
  else if (strcmp(field, "state") == 0)
    snprintf(
        out, len, "%s", d.player_state >= 0 && d.player_state < 3 ? STATES[d.player_state] : "?");
  else if (strcmp(field, "screen") == 0)
    snprintf(out, len, "%s", !d.is_open ? "apps" : d.is_now_playing ? "nowplaying" : "menu");
  else if (strcmp(field, "mode") == 0)
    snprintf(out, len, "%s", d.wheel_mode);
  else if (strcmp(field, "pos") == 0)
    snprintf(out, len, "%u", (unsigned)d.pos_ms);
  else if (strcmp(field, "dur") == 0)
    snprintf(out, len, "%u", (unsigned)d.dur_ms);
  else if (strcmp(field, "qpos") == 0)
    snprintf(out, len, "%zu", d.queue_pos);
  else if (strcmp(field, "qcount") == 0)
    snprintf(out, len, "%zu", d.queue_count);
  else if (strcmp(field, "volume") == 0)
    snprintf(out, len, "%d", d.volume);
  else if (strcmp(field, "rating") == 0)
    snprintf(out, len, "%d", d.rating);
  else if (strcmp(field, "shuffle") == 0)
    snprintf(out, len, "%d", d.shuffle);
  else if (strcmp(field, "repeat") == 0)
    snprintf(out, len, "%d", d.repeat);
  else if (strcmp(field, "eq") == 0)
    snprintf(out, len, "%d", d.eq);
  else if (strcmp(field, "otg") == 0)
    snprintf(out, len, "%zu", d.otg_count);
  else if (strcmp(field, "library") == 0)
    snprintf(out, len, "%zu", d.library_count);
  else if (strcmp(field, "hold") == 0)
    snprintf(out, len, "%d", (int)d.is_hold);
  else if (strcmp(field, "scanning") == 0)
    snprintf(out, len, "%d", (int)d.is_scanning);
  else if (strcmp(field, "art") == 0)
    snprintf(out, len, "%d", (int)d.is_art_shown);
  else if (strcmp(field, "overlay") == 0)
    snprintf(out, len, "%s", d.overlay);
  else if (strcmp(field, "streaming") == 0)
    snprintf(out, len, "%d", (int)a.streaming);
  else if (strcmp(field, "rate") == 0)
    snprintf(out, len, "%u", (unsigned)a.last_rate);
  else if (strcmp(field, "speaker_volume") == 0)
    snprintf(out, len, "%u", (unsigned)a.volume_pct);
  else
    snprintf(out, len, "<unknown field %s>", field);
}

static bool compare(const char *actual, const char *op, const char *expected) {
  if (strcmp(op, "=") == 0)
    return strcmp(actual, expected) == 0;
  if (strcmp(op, "!=") == 0)
    return strcmp(actual, expected) != 0;
  if (strcmp(op, "~") == 0)
    return strstr(actual, expected) != NULL;
  const long a = strtol(actual, NULL, 10);
  const long e = strtol(expected, NULL, 10);
  if (strcmp(op, ">") == 0)
    return a > e;
  if (strcmp(op, "<") == 0)
    return a < e;
  if (strcmp(op, ">=") == 0)
    return a >= e;
  if (strcmp(op, "<=") == 0)
    return a <= e;
  return false;
}

// Split "FIELD OP rest-of-line" (the value may contain spaces).
static bool parse_expect(char *args, char **field, char **op, char **value) {
  *field = strtok(args, " ");
  *op = strtok(NULL, " ");
  *value = strtok(NULL, "");
  if (*value == NULL)
    *value = "";
  return *field != NULL && *op != NULL;
}

// Run script lines until one has to wait. Returns false when the script ends.
static bool script_step(uint32_t now) {
  script_t *sc = s_script;
  if (sc->is_polling) {
    char line[SCRIPT_LINE];
    snprintf(line, sizeof(line), "%s", sc->lines[sc->pc]);
    char *args = line + strlen("wait_until ");
    char *field;
    char *op;
    char *value;
    parse_expect(args, &field, &op, &value);
    char *timeout_s = strrchr(value, ' ');
    if (timeout_s != NULL)
      *timeout_s = '\0';
    char actual[256];
    field_value(field, actual, sizeof(actual));
    if (compare(actual, op, value)) {
      sc->is_polling = false;
      sc->pc++;
    } else if ((int32_t)(now - sc->poll_deadline) >= 0) {
      fprintf(stderr,
              "script:%d: TIMEOUT %s %s '%s' (actual '%s')\n",
              sc->pc + 1,
              field,
              op,
              value,
              actual);
      sc->failures++;
      sc->is_polling = false;
      sc->pc++;
    } else {
      return true;
    }
  }
  if ((int32_t)(now - sc->wait_until) < 0)
    return true;

  if (sc->find_phase != 0) {
    // find TEXT / select TEXT: walk the highlighted list one row at a time.
    if (sc->find_release != INPUT_BTN_COUNT) {
      hb_input_set(sc->find_release, false, now);
      sc->find_release = INPUT_BTN_COUNT;
      sc->wait_until = now + FIND_STEP_MS;
      return true;
    }
    char line[SCRIPT_LINE];
    snprintf(line, sizeof(line), "%s", sc->lines[sc->pc]);
    const bool is_select = strncmp(line, "select ", 7) == 0;
    const char *target = line + (is_select ? 7 : 5);
    char sel[256];
    char idx_s[32];
    field_value("selected", sel, sizeof(sel));
    field_value("index", idx_s, sizeof(idx_s));
    const int idx = atoi(idx_s);
    if (sc->find_phase == 1 && idx == 0)
      sc->find_phase = 2;
    if (sc->find_phase == 2 && strcmp(sel, target) == 0) {
      sc->find_phase = 0;
      sc->pc++;
      if (is_select) {
        snprintf(sc->lines[sc->pc - 1], SCRIPT_LINE, "release ok");
        sc->pc--;
        hb_input_set(INPUT_BTN_OK, true, now);
        sc->wait_until = now + 60;
      }
      return true;
    }
    const bool stuck = sc->find_phase == 2 && idx == sc->find_last_index && sc->find_steps > 0;
    if (stuck || ++sc->find_steps > FIND_MAX_STEPS) {
      fprintf(stderr, "script:%d: row '%s' not found (at '%s')\n", sc->pc + 1, target, sel);
      sc->failures++;
      sc->find_phase = 0;
      sc->pc++;
      return true;
    }
    if (sc->find_phase == 2)
      sc->find_last_index = idx;
    sc->find_release = sc->find_phase == 1 ? INPUT_BTN_UP : INPUT_BTN_DOWN;
    hb_input_set(sc->find_release, true, now);
    sc->wait_until = now + FIND_STEP_MS;
    return true;
  }

  while (sc->pc < sc->count) {
    char line[SCRIPT_LINE];
    snprintf(line, sizeof(line), "%s", sc->lines[sc->pc]);
    char *cmd = line;
    while (isspace((unsigned char)*cmd))
      cmd++;
    if (*cmd == '\0' || *cmd == '#') {
      sc->pc++;
      continue;
    }
    char *args = strchr(cmd, ' ');
    if (args != NULL)
      *args++ = '\0';
    else
      args = cmd + strlen(cmd);

    if (strcmp(cmd, "press") == 0 || strcmp(cmd, "hold") == 0) {
      char *bname = strtok(args, " ");
      char *ms_s = strtok(NULL, " ");
      input_button_t b = bname != NULL ? parse_button(bname) : INPUT_BTN_COUNT;
      uint32_t ms = ms_s != NULL ? (uint32_t)atoi(ms_s) : 60;
      if (b == INPUT_BTN_COUNT) {
        fprintf(stderr, "script:%d: bad button\n", sc->pc + 1);
        sc->failures++;
        sc->pc++;
        continue;
      }
      hb_input_set(b, true, now);
      // Replace this line with the matching release after the hold time.
      snprintf(sc->lines[sc->pc], SCRIPT_LINE, "release %s", bname);
      sc->wait_until = now + ms;
      return true;
    }
    if (strcmp(cmd, "release") == 0) {
      hb_input_set(parse_button(args), false, now);
      sc->pc++;
      sc->wait_until = now + 120; // let the UI react before the next step
      return true;
    }
    if (strcmp(cmd, "down") == 0 || strcmp(cmd, "up") == 0) {
      hb_input_set(parse_button(args), strcmp(cmd, "down") == 0, now);
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "find") == 0 || strcmp(cmd, "select") == 0) {
      sc->find_phase = 1;
      sc->find_steps = 0;
      sc->find_last_index = -1;
      sc->find_release = INPUT_BTN_COUNT;
      return true;
    }
    if (strcmp(cmd, "wait") == 0) {
      sc->wait_until = now + (uint32_t)atoi(args);
      sc->pc++;
      return true;
    }
    if (strcmp(cmd, "shot") == 0) {
      lv_refr_now(NULL);
      save_shot(args);
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "cp") == 0) { // cp SRC => DST (device or host paths, spaces allowed)
      char *src = args;
      char *arrow = strstr(args, " => ");
      char *dst = arrow != NULL ? arrow + 4 : NULL;
      if (arrow != NULL)
        *arrow = '\0';
      FILE *in = src != NULL ? fopen(src, "rb") : NULL;
      FILE *out = in != NULL && dst != NULL ? fopen(dst, "wb") : NULL;
      if (in == NULL || out == NULL) {
        fprintf(stderr, "script:%d: cp failed\n", sc->pc + 1);
        sc->failures++;
      } else {
        char buf[8192];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
          fwrite(buf, 1, got, out);
      }
      if (in != NULL)
        fclose(in);
      if (out != NULL)
        fclose(out);
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "mkdir") == 0) {
      mkdir(args, 0775);
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "log") == 0) {
      fprintf(stderr, "script: %s\n", args);
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "quit") == 0) {
      sc->pc = sc->count;
      return false;
    }
    if (strcmp(cmd, "expect") == 0) {
      char *field;
      char *op;
      char *value;
      if (!parse_expect(args, &field, &op, &value)) {
        fprintf(stderr, "script:%d: bad expect\n", sc->pc + 1);
        sc->failures++;
      } else {
        char actual[256];
        field_value(field, actual, sizeof(actual));
        if (!compare(actual, op, value)) {
          fprintf(stderr,
                  "script:%d: FAIL %s %s '%s' (actual '%s')\n",
                  sc->pc + 1,
                  field,
                  op,
                  value,
                  actual);
          sc->failures++;
        }
      }
      sc->pc++;
      continue;
    }
    if (strcmp(cmd, "wait_until") == 0) {
      char tmp[SCRIPT_LINE];
      snprintf(tmp, sizeof(tmp), "%s", args);
      char *last = strrchr(tmp, ' ');
      uint32_t timeout = last != NULL ? (uint32_t)atoi(last + 1) : 0;
      sc->poll_deadline = now + (timeout > 0 ? timeout : 5000);
      sc->is_polling = true;
      return true;
    }
    fprintf(stderr, "script:%d: unknown command '%s'\n", sc->pc + 1, cmd);
    sc->failures++;
    sc->pc++;
  }
  return false;
}

// ---------------------------------------------------------------------------

static void usage(const char *argv0) {
  fprintf(
      stderr,
      "usage: %s [--sd DIR] [--scale N] [--landscape] [--launcher] [--headless] [--script FILE]\n"
      "  --sd DIR      host folder used as the microSD card (default ./sdcard)\n"
      "  --landscape   rotate the display to 320x240\n"
      "  --launcher    start in the TentacleOS Apps menu instead of the iPod app\n"
      "  --headless    no window (for CI); combine with --script\n"
      "  --script FILE run an automated UI script and exit with its failure count\n"
      "env: HB_AUDIO=sdl|null|fast  HB_AUDIO_CAPTURE=out.wav  HB_LOG=error|warn|info|debug\n",
      argv0);
}

int main(int argc, char **argv) {
  const char *sd = "sdcard";
  const char *script_path = NULL;
  bool launcher = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--sd") == 0 && i + 1 < argc) {
      sd = argv[++i];
    } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
      s_scale = atoi(argv[++i]);
      if (s_scale < 1 || s_scale > 4)
        s_scale = 2;
    } else if (strcmp(argv[i], "--landscape") == 0) {
      LCD_W = PANEL_LONG; // TentacleOS's rotated display setting
      LCD_H = PANEL_SHORT;
    } else if (strcmp(argv[i], "--launcher") == 0) {
      launcher = true;
    } else if (strcmp(argv[i], "--headless") == 0) {
      s_headless = true;
    } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
      script_path = argv[++i];
    } else {
      usage(argv[0]);
      return 2;
    }
  }
  hb_fs_set_sd_root(sd);
  if (script_path != NULL && !script_load(script_path))
    return 2;

  if (!s_headless) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
      fprintf(stderr, "SDL_Init failed (%s); running headless\n", SDL_GetError());
      s_headless = true;
    } else {
      layout_hits();
      const int win_w = LCD_W * s_scale + 2 * BEZEL;
      const int win_h = LCD_H * s_scale + BEZEL + PAD_AREA_H;
      s_win = SDL_CreateWindow("High Boy - iPod (simulator)",
                               SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED,
                               win_w,
                               win_h,
                               0);
      s_ren = s_win != NULL ? SDL_CreateRenderer(s_win, -1, 0) : NULL;
      s_tex = s_ren != NULL
                  ? SDL_CreateTexture(
                        s_ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, LCD_W, LCD_H)
                  : NULL;
      if (s_tex == NULL) {
        fprintf(stderr, "SDL window failed (%s); running headless\n", SDL_GetError());
        s_headless = true;
      }
    }
  }

  lv_init();
  lv_tick_set_cb(now_ms);
  lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  static uint16_t draw_buf[PANEL_LONG * DRAW_BUF_LINES];
  lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  audio_i2s_init();
  input_manager_init();
  if (launcher)
    ui_switch_screen(SCREEN_GAMES_MENU);
  else
    ui_switch_screen(SCREEN_IPOD);

  bool running = true;
  while (running) {
    const uint32_t now = now_ms();
    if (!s_headless)
      running = handle_sdl_events();
    if (s_script != NULL && !script_step(now))
      running = false;
    hb_input_tick(now_ms());
    hb_ui_pump();
    lv_timer_handler();
    if (s_fb_dirty) {
      render_window();
      s_fb_dirty = false;
    } else if (!s_headless) {
      render_window(); // keep the pressed-button highlights live
    }
    if (hb_ui_quit_requested())
      running = false;
    usleep(LOOP_SLEEP_US);
  }

  // Leave like TentacleOS would: the screen's close hook releases everything.
  if (ui_current_screen() == SCREEN_IPOD)
    ui_ipod_stop();
  int failures = s_script != NULL ? s_script->failures : 0;
  if (s_script != NULL && s_script->pc < s_script->count) {
    fprintf(stderr, "script:%d: simulator exited before the script finished\n", s_script->pc + 1);
    failures++;
  }
  if (s_script != NULL)
    fprintf(stderr, "script finished: %d failure(s)\n", failures);
  free(s_script);
  lv_deinit();
  if (s_tex != NULL)
    SDL_DestroyTexture(s_tex);
  if (s_ren != NULL)
    SDL_DestroyRenderer(s_ren);
  if (s_win != NULL)
    SDL_DestroyWindow(s_win);
  SDL_Quit();
  return failures == 0 ? 0 : 1;
}
