// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef UI_IPOD_H
#define UI_IPOD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Open the iPod app (SCREEN_IPOD open function). */
void ui_ipod_open(void);

/** @brief Stop playback and release everything (SCREEN_IPOD close hook). */
void ui_ipod_stop(void);

/** @brief Screen to return to when leaving the app (default: the Apps menu). */
void ui_ipod_set_return(int screen);

/** @brief Snapshot of the app state, for tests and the debug console. */
typedef struct {
  bool is_open;
  bool is_now_playing; ///< the Now Playing screen is showing
  bool is_hold;        ///< Hold switch engaged
  bool is_scanning;
  char title[64];      ///< header title of the current screen
  char selected[128];  ///< label of the highlighted row
  int selected_index;
  int row_count;
  char wheel_mode[16]; ///< Now Playing wheel mode: volume, scrub, rating, shuffle
  char now_title[128]; ///< current song title ("" when nothing is loaded)
  int player_state;    ///< ipod_play_state_t
  uint32_t pos_ms;
  uint32_t dur_ms;
  size_t queue_pos;
  size_t queue_count;
  int volume;
  int rating;
  int shuffle;
  int repeat;
  int eq;
  size_t otg_count;
  size_t library_count;
  bool is_art_shown;
  char overlay[16]; ///< fast-scroll letter currently shown ("" if none)
} ui_ipod_debug_t;

void ui_ipod_debug(ui_ipod_debug_t *out);

#ifdef __cplusplus
}
#endif

#endif // UI_IPOD_H
