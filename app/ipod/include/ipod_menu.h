// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_MENU_H
#define IPOD_MENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipod_library.h"
#include "ipod_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_MENU_DEPTH     16
#define IPOD_MENU_TITLE_MAX 64
#define IPOD_ROW_TEXT_MAX   128
#define IPOD_ROW_RIGHT_MAX  32
#define IPOD_NO_TRACK       UINT32_MAX

/** @brief Every screen of the app. The last group is drawn specially. */
typedef enum {
  IPOD_VIEW_MAIN = 0,
  IPOD_VIEW_MUSIC,
  IPOD_VIEW_PLAYLISTS,
  IPOD_VIEW_ARTISTS,
  IPOD_VIEW_ALBUMS,
  IPOD_VIEW_SONGS,
  IPOD_VIEW_GENRES,
  IPOD_VIEW_COMPOSERS,
  IPOD_VIEW_EXTRAS,
  IPOD_VIEW_SETTINGS,
  IPOD_VIEW_EQ,
  IPOD_VIEW_VOLUME_LIMIT,
  IPOD_VIEW_ABOUT,
  IPOD_VIEW_RESET,
  // custom screens
  IPOD_VIEW_NOW_PLAYING,
  IPOD_VIEW_SEARCH,
  IPOD_VIEW_CLOCK,
  IPOD_VIEW_STOPWATCH,
  IPOD_VIEW_COUNT,
} ipod_view_kind_t;

typedef enum {
  IPOD_ROW_NAV = 0, ///< opens another view (sub = ipod_view_kind_t)
  IPOD_ROW_GROUP,   ///< artist / album / genre / composer (sub = field, value = rep track)
  IPOD_ROW_ALL,     ///< "All Songs" of the current filter
  IPOD_ROW_TRACK,   ///< value = index into view->tracks
  IPOD_ROW_PLAYLIST,
  IPOD_ROW_ACTION,
  IPOD_ROW_SETTING, ///< toggles/cycles in place
  IPOD_ROW_CHOICE,  ///< one option of a list (EQ, volume limit)
  IPOD_ROW_INFO,    ///< About page line
  IPOD_ROW_EMPTY,   ///< placeholder, not selectable
} ipod_row_kind_t;

typedef enum {
  IPOD_PL_ON_THE_GO = 0,
  IPOD_PL_TOP_RATED,
  IPOD_PL_RECENTLY_ADDED,
  IPOD_PL_RECENTLY_PLAYED,
  IPOD_PL_TOP_PLAYED,
  IPOD_PL_M3U, ///< value = playlist index in the library
} ipod_playlist_kind_t;

typedef enum {
  IPOD_ACTION_SHUFFLE_SONGS = 0,
  IPOD_ACTION_UPDATE_LIBRARY,
  IPOD_ACTION_CLEAR_OTG,
  IPOD_ACTION_RESET_CANCEL,
  IPOD_ACTION_RESET_CONFIRM,
} ipod_action_id_t;

typedef enum {
  IPOD_SETTING_SHUFFLE = 0,
  IPOD_SETTING_REPEAT,
  IPOD_SETTING_SOUND_CHECK,
  IPOD_SETTING_CLICKER,
  IPOD_SETTING_EQ,
  IPOD_SETTING_VOLUME_LIMIT,
  IPOD_SETTING_ALL, ///< reset
} ipod_setting_id_t;

typedef enum {
  IPOD_INFO_SONGS = 0,
  IPOD_INFO_ALBUMS,
  IPOD_INFO_ARTISTS,
  IPOD_INFO_GENRES,
  IPOD_INFO_PLAYLISTS,
  IPOD_INFO_PLAYTIME,
  IPOD_INFO_SIZE,
  IPOD_INFO_FORMAT,
  IPOD_INFO_VERSION,
  IPOD_INFO_COUNT,
} ipod_info_id_t;

typedef struct {
  uint8_t kind; ///< ipod_row_kind_t
  uint8_t sub;
  uint32_t value;
} ipod_row_t;

typedef struct {
  ipod_view_kind_t kind;
  char title[IPOD_MENU_TITLE_MAX];
  ipod_filter_t filter;
  ipod_row_t *rows;
  size_t nrows;
  uint32_t *tracks; ///< song list of a SONGS view (owned)
  size_t ntracks;
  bool is_otg;
  bool has_letters; ///< sorted by name: fast scrolling shows index letters
  int sel;
  int top;
} ipod_view_t;

typedef struct {
  ipod_library_t *lib;
  ipod_settings_t *settings;
  ipod_view_t stack[IPOD_MENU_DEPTH];
  int depth;
  bool has_now_playing; ///< shows "Now Playing" in the main menu
  uint32_t *otg;
  size_t otg_count;
  size_t otg_cap;
} ipod_menu_t;

typedef enum {
  IPOD_MENU_NONE = 0,
  IPOD_MENU_PUSHED,
  IPOD_MENU_PLAY,        ///< play tracks[start..] (then show Now Playing)
  IPOD_MENU_SHUFFLE_ALL, ///< "Shuffle Songs"
  IPOD_MENU_SETTING,     ///< settings changed (setting = ipod_setting_id_t)
  IPOD_MENU_UPDATE_LIBRARY,
  IPOD_MENU_OTG_CHANGED,
  IPOD_MENU_POPPED,
} ipod_menu_event_t;

typedef struct {
  ipod_menu_event_t event;
  const uint32_t *tracks; ///< valid until the view changes
  size_t count;
  size_t start;
  int setting;
} ipod_menu_result_t;

typedef enum {
  IPOD_ICON_NONE = 0,
  IPOD_ICON_ARROW,
  IPOD_ICON_CHECK,
  IPOD_ICON_SPEAKER,
} ipod_row_icon_t;

typedef struct {
  char text[IPOD_ROW_TEXT_MAX];
  char right[IPOD_ROW_RIGHT_MAX];
  ipod_row_icon_t icon;
  bool is_selectable;
} ipod_row_text_t;

void ipod_menu_init(ipod_menu_t *m, ipod_library_t *lib, ipod_settings_t *settings);
void ipod_menu_free(ipod_menu_t *m);

/** @brief Clear the stack and show the main menu. */
void ipod_menu_reset(ipod_menu_t *m);
/** @brief Point at a new library (after a rescan); returns to the main menu. */
void ipod_menu_set_library(ipod_menu_t *m, ipod_library_t *lib);

ipod_view_t *ipod_menu_top(ipod_menu_t *m);

/** @brief Open a view on top of the stack (custom screens included). */
bool ipod_menu_push(ipod_menu_t *m, ipod_view_kind_t kind, const char *title);

/** @brief Go back one level. Returns false at the main menu. */
bool ipod_menu_back(ipod_menu_t *m);

/** @brief Rebuild the top view's rows (e.g. after settings or OTG changed). */
void ipod_menu_refresh(ipod_menu_t *m);

/** @brief Move the highlight by @p delta rows, keeping it on screen. */
void ipod_menu_move(ipod_menu_t *m, int delta, int visible_rows);

/** @brief Act on the highlighted row (the wheel's centre button). */
ipod_menu_result_t ipod_menu_select(ipod_menu_t *m);

/**
 * @brief Holding the centre button: add the highlighted song / album / artist /
 *        playlist to On-The-Go. Returns the number of songs added.
 */
size_t ipod_menu_hold_select(ipod_menu_t *m);

/** @brief Open an artist / album / genre / composer by a representative track. */
bool ipod_menu_open_group(ipod_menu_t *m, ipod_field_t field, uint32_t track);

/** @brief Text of a row; @p playing_track marks the song that is playing. */
void ipod_menu_row_text(const ipod_menu_t *m,
                        const ipod_view_t *v,
                        size_t row,
                        uint32_t playing_track,
                        ipod_row_text_t *out);

/** @brief Index letter for fast scrolling ('A'..'Z', '#'), 0 if none. */
char ipod_menu_row_letter(const ipod_menu_t *m, const ipod_view_t *v, size_t row);

/** @brief Album id per track, for album shuffle (caller frees). */
uint32_t *ipod_menu_album_groups(const ipod_menu_t *m, const uint32_t *tracks, size_t count);

/** @brief Append songs to On-The-Go. Returns how many were added. */
size_t ipod_menu_otg_add(ipod_menu_t *m, const uint32_t *tracks, size_t count);

/** @brief Replace the On-The-Go list (e.g. loaded from the card). */
void ipod_menu_set_otg(ipod_menu_t *m, const uint32_t *tracks, size_t count);

const char *ipod_shuffle_name(ipod_shuffle_t s);
const char *ipod_repeat_name(ipod_repeat_t r);

#ifdef __cplusplus
}
#endif

#endif // IPOD_MENU_H
