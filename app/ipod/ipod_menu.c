// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "ipod_dsp.h"
#include "ipod_text.h"

#define IPOD_APP_VERSION  "1.0.0"
#define OTG_INITIAL       32
#define ROWS_INITIAL      16
#define COMPILATIONS_FLAG 1u
#define MS_PER_MIN        60000u
#define MIN_PER_HOUR      60u
#define BYTES_PER_MB      (1024u * 1024u)

static const char *TAG = "IPOD_MENU";

static const uint8_t VOLUME_LIMITS[] = {100, 90, 80, 70, 60, 50, 40, 30};

typedef struct {
  ipod_row_t *rows;
  size_t n;
  size_t cap;
  bool is_oom;
} row_builder_t;

static void build_view(ipod_menu_t *m, ipod_view_t *v);

static void add_row(row_builder_t *b, ipod_row_kind_t kind, uint8_t sub, uint32_t value) {
  if (b->is_oom)
    return;
  if (b->n == b->cap) {
    size_t cap = b->cap == 0 ? ROWS_INITIAL : b->cap * 2;
    ipod_row_t *grown = realloc(b->rows, cap * sizeof(ipod_row_t));
    if (grown == NULL) {
      ESP_LOGE(TAG, "row alloc failed");
      b->is_oom = true;
      return;
    }
    b->rows = grown;
    b->cap = cap;
  }
  b->rows[b->n++] = (ipod_row_t){.kind = (uint8_t)kind, .sub = sub, .value = value};
}

static void free_view(ipod_view_t *v) {
  free(v->rows);
  free(v->tracks);
  memset(v, 0, sizeof(*v));
}

void ipod_menu_init(ipod_menu_t *m, ipod_library_t *lib, ipod_settings_t *settings) {
  memset(m, 0, sizeof(*m));
  m->lib = lib;
  m->settings = settings;
  ipod_menu_reset(m);
}

void ipod_menu_free(ipod_menu_t *m) {
  for (int i = 0; i < m->depth; i++)
    free_view(&m->stack[i]);
  m->depth = 0;
  free(m->otg);
  m->otg = NULL;
  m->otg_count = 0;
  m->otg_cap = 0;
}

void ipod_menu_reset(ipod_menu_t *m) {
  for (int i = 0; i < m->depth; i++)
    free_view(&m->stack[i]);
  m->depth = 0;
  ipod_menu_push(m, IPOD_VIEW_MAIN, "iPod");
}

void ipod_menu_set_library(ipod_menu_t *m, ipod_library_t *lib) {
  m->lib = lib;
  m->otg_count = 0; // track indices belong to the old library
  ipod_menu_reset(m);
}

ipod_view_t *ipod_menu_top(ipod_menu_t *m) {
  return m->depth > 0 ? &m->stack[m->depth - 1] : NULL;
}

static ipod_view_t *push_raw(ipod_menu_t *m, ipod_view_kind_t kind, const char *title) {
  if (m->depth >= IPOD_MENU_DEPTH) {
    ESP_LOGW(TAG, "menu stack full");
    return NULL;
  }
  ipod_view_t *v = &m->stack[m->depth++];
  memset(v, 0, sizeof(*v));
  v->kind = kind;
  ipod_text_fold(title, v->title, sizeof(v->title));
  return v;
}

bool ipod_menu_push(ipod_menu_t *m, ipod_view_kind_t kind, const char *title) {
  ipod_view_t *v = push_raw(m, kind, title);
  if (v == NULL)
    return false;
  build_view(m, v);
  return true;
}

static bool push_filtered(ipod_menu_t *m,
                          ipod_view_kind_t kind,
                          const char *title,
                          const ipod_filter_t *filter) {
  ipod_view_t *v = push_raw(m, kind, title);
  if (v == NULL)
    return false;
  if (filter != NULL)
    v->filter = *filter;
  build_view(m, v);
  return true;
}

// Takes ownership of @p tracks.
static bool push_songs(
    ipod_menu_t *m, const char *title, uint32_t *tracks, size_t n, bool has_letters, bool is_otg) {
  ipod_view_t *v = push_raw(m, IPOD_VIEW_SONGS, title);
  if (v == NULL) {
    free(tracks);
    return false;
  }
  v->tracks = tracks;
  v->ntracks = n;
  v->has_letters = has_letters;
  v->is_otg = is_otg;
  build_view(m, v);
  return true;
}

bool ipod_menu_back(ipod_menu_t *m) {
  if (m->depth <= 1)
    return false;
  free_view(&m->stack[m->depth - 1]);
  m->depth--;
  return true;
}

static void clamp_selection(ipod_view_t *v) {
  if (v->nrows == 0) {
    v->sel = 0;
    v->top = 0;
    return;
  }
  if (v->sel >= (int)v->nrows)
    v->sel = (int)v->nrows - 1;
  if (v->sel < 0)
    v->sel = 0;
  if (v->top > v->sel)
    v->top = v->sel;
}

static uint32_t *copy_otg(const ipod_menu_t *m) {
  if (m->otg_count == 0)
    return NULL;
  uint32_t *t = malloc(m->otg_count * sizeof(uint32_t));
  if (t == NULL) {
    ESP_LOGE(TAG, "otg copy failed");
    return NULL;
  }
  memcpy(t, m->otg, m->otg_count * sizeof(uint32_t));
  return t;
}

void ipod_menu_refresh(ipod_menu_t *m) {
  ipod_view_t *v = ipod_menu_top(m);
  if (v == NULL)
    return;
  if (v->is_otg) {
    free(v->tracks);
    v->tracks = copy_otg(m);
    v->ntracks = v->tracks != NULL ? m->otg_count : 0;
  }
  free(v->rows);
  v->rows = NULL;
  v->nrows = 0;
  build_view(m, v);
}

void ipod_menu_move(ipod_menu_t *m, int delta, int visible_rows) {
  ipod_view_t *v = ipod_menu_top(m);
  if (v == NULL || v->nrows == 0)
    return;
  v->sel += delta;
  clamp_selection(v);
  if (visible_rows < 1)
    visible_rows = 1;
  if (v->sel < v->top)
    v->top = v->sel;
  if (v->sel >= v->top + visible_rows)
    v->top = v->sel - visible_rows + 1;
  int max_top = (int)v->nrows - visible_rows;
  if (max_top < 0)
    max_top = 0;
  if (v->top > max_top)
    v->top = max_top;
}

// ---------------------------------------------------------------------------
// Building views

static void build_groups(ipod_menu_t *m, ipod_view_t *v, ipod_field_t field, row_builder_t *b) {
  uint32_t *reps = NULL;
  size_t n = ipod_lib_query_groups(m->lib, field, &v->filter, &reps);
  const bool filtered = v->filter.album_artist != IPOD_STR_EMPTY ||
                        v->filter.genre != IPOD_STR_EMPTY || v->filter.compilations_only;
  if (field == IPOD_FIELD_ALBUM && filtered && n > 0)
    add_row(b, IPOD_ROW_ALL, 0, 0);
  for (size_t i = 0; i < n; i++)
    add_row(b, IPOD_ROW_GROUP, (uint8_t)field, reps[i]);
  free(reps);
  v->has_letters = true;
}

static void build_view(ipod_menu_t *m, ipod_view_t *v) {
  row_builder_t b = {0};
  switch (v->kind) {
    case IPOD_VIEW_MAIN:
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_MUSIC, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_EXTRAS, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_SETTINGS, 0);
      add_row(&b, IPOD_ROW_ACTION, IPOD_ACTION_SHUFFLE_SONGS, 0);
      if (m->has_now_playing)
        add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_NOW_PLAYING, 0);
      break;
    case IPOD_VIEW_MUSIC: {
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_PLAYLISTS, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_ARTISTS, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_ALBUMS, 0);
      if (ipod_lib_compilation_count(m->lib) > 0)
        add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_ALBUMS, COMPILATIONS_FLAG);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_SONGS, 0);
      uint32_t *tmp = NULL;
      if (ipod_lib_query_groups(m->lib, IPOD_FIELD_GENRE, NULL, &tmp) > 0)
        add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_GENRES, 0);
      free(tmp);
      tmp = NULL;
      if (ipod_lib_query_groups(m->lib, IPOD_FIELD_COMPOSER, NULL, &tmp) > 0)
        add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_COMPOSERS, 0);
      free(tmp);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_SEARCH, 0);
      break;
    }
    case IPOD_VIEW_PLAYLISTS:
      add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_TOP_RATED, 0);
      add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_RECENTLY_ADDED, 0);
      add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_RECENTLY_PLAYED, 0);
      add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_TOP_PLAYED, 0);
      for (size_t i = 0; i < ipod_lib_playlist_count(m->lib); i++)
        add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_M3U, (uint32_t)i);
      add_row(&b, IPOD_ROW_PLAYLIST, IPOD_PL_ON_THE_GO, 0);
      break;
    case IPOD_VIEW_ARTISTS:
      build_groups(m, v, IPOD_FIELD_ARTIST, &b);
      break;
    case IPOD_VIEW_ALBUMS:
      build_groups(m, v, IPOD_FIELD_ALBUM, &b);
      break;
    case IPOD_VIEW_GENRES:
      build_groups(m, v, IPOD_FIELD_GENRE, &b);
      break;
    case IPOD_VIEW_COMPOSERS:
      build_groups(m, v, IPOD_FIELD_COMPOSER, &b);
      break;
    case IPOD_VIEW_SONGS:
      for (size_t i = 0; i < v->ntracks; i++)
        add_row(&b, IPOD_ROW_TRACK, 0, (uint32_t)i);
      if (v->is_otg && v->ntracks > 0)
        add_row(&b, IPOD_ROW_ACTION, IPOD_ACTION_CLEAR_OTG, 0);
      if (v->ntracks == 0)
        add_row(&b, IPOD_ROW_EMPTY, 0, 0);
      break;
    case IPOD_VIEW_EXTRAS:
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_CLOCK, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_STOPWATCH, 0);
      break;
    case IPOD_VIEW_SETTINGS:
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_ABOUT, 0);
      add_row(&b, IPOD_ROW_SETTING, IPOD_SETTING_SHUFFLE, 0);
      add_row(&b, IPOD_ROW_SETTING, IPOD_SETTING_REPEAT, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_VOLUME_LIMIT, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_EQ, 0);
      add_row(&b, IPOD_ROW_SETTING, IPOD_SETTING_SOUND_CHECK, 0);
      add_row(&b, IPOD_ROW_SETTING, IPOD_SETTING_CLICKER, 0);
      add_row(&b, IPOD_ROW_ACTION, IPOD_ACTION_UPDATE_LIBRARY, 0);
      add_row(&b, IPOD_ROW_NAV, IPOD_VIEW_RESET, 0);
      break;
    case IPOD_VIEW_EQ:
      for (int i = 0; i < ipod_eq_count(); i++)
        add_row(&b, IPOD_ROW_CHOICE, IPOD_SETTING_EQ, (uint32_t)i);
      if (v->nrows == 0 && m->settings != NULL)
        v->sel = m->settings->eq_preset;
      break;
    case IPOD_VIEW_VOLUME_LIMIT:
      for (size_t i = 0; i < sizeof(VOLUME_LIMITS); i++) {
        add_row(&b, IPOD_ROW_CHOICE, IPOD_SETTING_VOLUME_LIMIT, VOLUME_LIMITS[i]);
        if (m->settings != NULL && m->settings->volume_limit == VOLUME_LIMITS[i])
          v->sel = (int)i;
      }
      break;
    case IPOD_VIEW_ABOUT:
      for (int i = 0; i < IPOD_INFO_COUNT; i++)
        add_row(&b, IPOD_ROW_INFO, (uint8_t)i, 0);
      break;
    case IPOD_VIEW_RESET:
      add_row(&b, IPOD_ROW_ACTION, IPOD_ACTION_RESET_CANCEL, 0);
      add_row(&b, IPOD_ROW_ACTION, IPOD_ACTION_RESET_CONFIRM, 0);
      break;
    default:
      break; // custom screens have no rows
  }
  v->rows = b.rows;
  v->nrows = b.n;
  clamp_selection(v);
}

// ---------------------------------------------------------------------------
// Selection

static size_t playlist_tracks(const ipod_menu_t *m, const ipod_row_t *row, uint32_t **out) {
  *out = NULL;
  switch ((ipod_playlist_kind_t)row->sub) {
    case IPOD_PL_ON_THE_GO:
      *out = copy_otg(m);
      return *out != NULL ? m->otg_count : 0;
    case IPOD_PL_TOP_RATED:
      return ipod_lib_smart(m->lib, IPOD_SMART_TOP_RATED, out);
    case IPOD_PL_RECENTLY_ADDED:
      return ipod_lib_smart(m->lib, IPOD_SMART_RECENTLY_ADDED, out);
    case IPOD_PL_RECENTLY_PLAYED:
      return ipod_lib_smart(m->lib, IPOD_SMART_RECENTLY_PLAYED, out);
    case IPOD_PL_TOP_PLAYED:
      return ipod_lib_smart(m->lib, IPOD_SMART_TOP_PLAYED, out);
    case IPOD_PL_M3U:
      return ipod_lib_playlist_tracks(m->lib, row->value, out);
    default:
      return 0;
  }
}

static const char *playlist_name(const ipod_menu_t *m, const ipod_row_t *row) {
  switch ((ipod_playlist_kind_t)row->sub) {
    case IPOD_PL_ON_THE_GO:
      return "On-The-Go";
    case IPOD_PL_TOP_RATED:
      return "My Top Rated";
    case IPOD_PL_RECENTLY_ADDED:
      return "Recently Added";
    case IPOD_PL_RECENTLY_PLAYED:
      return "Recently Played";
    case IPOD_PL_TOP_PLAYED:
      return "Top 25 Most Played";
    case IPOD_PL_M3U:
      return ipod_lib_playlist_name(m->lib, row->value);
    default:
      return "";
  }
}

static const char *view_name(ipod_view_kind_t kind, uint32_t flags) {
  switch (kind) {
    case IPOD_VIEW_MUSIC:
      return "Music";
    case IPOD_VIEW_PLAYLISTS:
      return "Playlists";
    case IPOD_VIEW_ARTISTS:
      return "Artists";
    case IPOD_VIEW_ALBUMS:
      return flags == COMPILATIONS_FLAG ? "Compilations" : "Albums";
    case IPOD_VIEW_SONGS:
      return "Songs";
    case IPOD_VIEW_GENRES:
      return "Genres";
    case IPOD_VIEW_COMPOSERS:
      return "Composers";
    case IPOD_VIEW_EXTRAS:
      return "Extras";
    case IPOD_VIEW_SETTINGS:
      return "Settings";
    case IPOD_VIEW_EQ:
      return "EQ";
    case IPOD_VIEW_VOLUME_LIMIT:
      return "Volume Limit";
    case IPOD_VIEW_ABOUT:
      return "About";
    case IPOD_VIEW_RESET:
      return "Reset Settings";
    case IPOD_VIEW_NOW_PLAYING:
      return "Now Playing";
    case IPOD_VIEW_SEARCH:
      return "Search...";
    case IPOD_VIEW_CLOCK:
      return "Clock";
    case IPOD_VIEW_STOPWATCH:
      return "Stopwatch";
    default:
      return "iPod";
  }
}

// Filter for drilling into a group row.
static ipod_filter_t
group_filter(const ipod_menu_t *m, const ipod_view_t *v, const ipod_row_t *row) {
  ipod_filter_t f = v->filter;
  const ipod_track_t *t = ipod_lib_track(m->lib, row->value);
  if (t == NULL)
    return f;
  switch ((ipod_field_t)row->sub) {
    case IPOD_FIELD_ARTIST:
      f.album_artist = t->album_artist;
      break;
    case IPOD_FIELD_ALBUM:
      f.album = t->album;
      f.album_artist = t->album_artist;
      break;
    case IPOD_FIELD_GENRE:
      f.genre = t->genre;
      break;
    case IPOD_FIELD_COMPOSER:
      f.composer = t->composer;
      break;
    default:
      break;
  }
  return f;
}

static ipod_menu_result_t select_group(ipod_menu_t *m, ipod_view_t *v, const ipod_row_t *row) {
  ipod_menu_result_t r = {.event = IPOD_MENU_PUSHED};
  ipod_filter_t f = group_filter(m, v, row);
  const ipod_field_t field = (ipod_field_t)row->sub;
  char title[IPOD_ROW_TEXT_MAX];
  ipod_text_copy(
      title, sizeof(title), ipod_lib_str(m->lib, ipod_lib_field_value(m->lib, field, row->value)));
  switch (field) {
    case IPOD_FIELD_ARTIST:
      push_filtered(m, IPOD_VIEW_ALBUMS, title, &f);
      break;
    case IPOD_FIELD_GENRE:
      push_filtered(m, IPOD_VIEW_ARTISTS, title, &f);
      break;
    case IPOD_FIELD_ALBUM:
    case IPOD_FIELD_COMPOSER: {
      uint32_t *tracks = NULL;
      size_t n = ipod_lib_query_tracks(m->lib, &f, IPOD_ORDER_ALBUM, &tracks);
      if (push_songs(m, title, tracks, n, false, false))
        ipod_menu_top(m)->filter = f;
      else
        r.event = IPOD_MENU_NONE;
      break;
    }
    default:
      r.event = IPOD_MENU_NONE;
      break;
  }
  return r;
}

bool ipod_menu_open_group(ipod_menu_t *m, ipod_field_t field, uint32_t track) {
  ipod_view_t base = {0}; // no inherited filter
  const ipod_row_t row = {.kind = IPOD_ROW_GROUP, .sub = (uint8_t)field, .value = track};
  return select_group(m, &base, &row).event == IPOD_MENU_PUSHED;
}

static void apply_cycle(ipod_settings_t *s, ipod_setting_id_t id) {
  switch (id) {
    case IPOD_SETTING_SHUFFLE:
      s->shuffle = (ipod_shuffle_t)((s->shuffle + 1) % IPOD_SHUFFLE_COUNT);
      break;
    case IPOD_SETTING_REPEAT:
      s->repeat = (ipod_repeat_t)((s->repeat + 1) % IPOD_REPEAT_COUNT);
      break;
    case IPOD_SETTING_SOUND_CHECK:
      s->sound_check = !s->sound_check;
      break;
    case IPOD_SETTING_CLICKER:
      s->clicker = !s->clicker;
      break;
    default:
      break;
  }
}

ipod_menu_result_t ipod_menu_select(ipod_menu_t *m) {
  ipod_menu_result_t r = {.event = IPOD_MENU_NONE};
  ipod_view_t *v = ipod_menu_top(m);
  if (v == NULL || v->nrows == 0 || v->sel < 0 || v->sel >= (int)v->nrows)
    return r;
  const ipod_row_t row = v->rows[v->sel];
  switch ((ipod_row_kind_t)row.kind) {
    case IPOD_ROW_NAV: {
      const ipod_view_kind_t target = (ipod_view_kind_t)row.sub;
      const char *name = view_name(target, row.value);
      if (target == IPOD_VIEW_SONGS) {
        uint32_t *tracks = NULL;
        size_t n = ipod_lib_query_tracks(m->lib, NULL, IPOD_ORDER_TITLE, &tracks);
        push_songs(m, name, tracks, n, true, false);
      } else if (target == IPOD_VIEW_ALBUMS && row.value == COMPILATIONS_FLAG) {
        ipod_filter_t f = {.compilations_only = true};
        push_filtered(m, IPOD_VIEW_ALBUMS, name, &f);
      } else if (target == IPOD_VIEW_SEARCH) {
        ipod_menu_push(m, target, "Search");
      } else {
        ipod_menu_push(m, target, name);
      }
      r.event = IPOD_MENU_PUSHED;
      break;
    }
    case IPOD_ROW_GROUP:
      r = select_group(m, v, &row);
      break;
    case IPOD_ROW_ALL: {
      uint32_t *tracks = NULL;
      ipod_filter_t f = v->filter;
      size_t n = ipod_lib_query_tracks(m->lib, &f, IPOD_ORDER_ALBUM, &tracks);
      char title[IPOD_MENU_TITLE_MAX];
      ipod_text_copy(title, sizeof(title), v->title);
      if (push_songs(m, title, tracks, n, false, false)) {
        ipod_menu_top(m)->filter = f;
        r.event = IPOD_MENU_PUSHED;
      }
      break;
    }
    case IPOD_ROW_TRACK:
      r.event = IPOD_MENU_PLAY;
      r.tracks = v->tracks;
      r.count = v->ntracks;
      r.start = row.value;
      break;
    case IPOD_ROW_PLAYLIST: {
      uint32_t *tracks = NULL;
      size_t n = playlist_tracks(m, &row, &tracks);
      char title[IPOD_ROW_TEXT_MAX];
      ipod_text_copy(title, sizeof(title), playlist_name(m, &row));
      push_songs(m, title, tracks, n, false, row.sub == IPOD_PL_ON_THE_GO);
      r.event = IPOD_MENU_PUSHED;
      break;
    }
    case IPOD_ROW_ACTION:
      switch ((ipod_action_id_t)row.sub) {
        case IPOD_ACTION_SHUFFLE_SONGS:
          r.event = IPOD_MENU_SHUFFLE_ALL;
          break;
        case IPOD_ACTION_UPDATE_LIBRARY:
          r.event = IPOD_MENU_UPDATE_LIBRARY;
          break;
        case IPOD_ACTION_CLEAR_OTG:
          m->otg_count = 0;
          ipod_menu_refresh(m);
          r.event = IPOD_MENU_OTG_CHANGED;
          break;
        case IPOD_ACTION_RESET_CANCEL:
          ipod_menu_back(m);
          r.event = IPOD_MENU_POPPED;
          break;
        case IPOD_ACTION_RESET_CONFIRM:
          if (m->settings != NULL)
            ipod_settings_defaults(m->settings);
          ipod_menu_back(m);
          r.event = IPOD_MENU_SETTING;
          r.setting = IPOD_SETTING_ALL;
          break;
        default:
          break;
      }
      break;
    case IPOD_ROW_SETTING:
      if (m->settings != NULL) {
        apply_cycle(m->settings, (ipod_setting_id_t)row.sub);
        r.event = IPOD_MENU_SETTING;
        r.setting = row.sub;
      }
      break;
    case IPOD_ROW_CHOICE:
      if (m->settings != NULL) {
        if (row.sub == IPOD_SETTING_EQ) {
          m->settings->eq_preset = (int)row.value;
        } else {
          m->settings->volume_limit = (uint8_t)row.value;
          if (m->settings->volume > m->settings->volume_limit)
            m->settings->volume = m->settings->volume_limit;
        }
        ipod_menu_back(m); // the iPod returns to Settings after a pick
        r.event = IPOD_MENU_SETTING;
        r.setting = row.sub;
      }
      break;
    default:
      break;
  }
  return r;
}

static size_t otg_append(ipod_menu_t *m, const uint32_t *tracks, size_t n) {
  if (n == 0 || tracks == NULL)
    return 0;
  if (m->otg_count + n > m->otg_cap) {
    size_t cap = m->otg_cap == 0 ? OTG_INITIAL : m->otg_cap;
    while (cap < m->otg_count + n)
      cap *= 2;
    uint32_t *grown = realloc(m->otg, cap * sizeof(uint32_t));
    if (grown == NULL) {
      ESP_LOGE(TAG, "otg grow failed");
      return 0;
    }
    m->otg = grown;
    m->otg_cap = cap;
  }
  memcpy(m->otg + m->otg_count, tracks, n * sizeof(uint32_t));
  m->otg_count += n;
  return n;
}

size_t ipod_menu_otg_add(ipod_menu_t *m, const uint32_t *tracks, size_t count) {
  return otg_append(m, tracks, count);
}

void ipod_menu_set_otg(ipod_menu_t *m, const uint32_t *tracks, size_t count) {
  m->otg_count = 0;
  otg_append(m, tracks, count);
}

size_t ipod_menu_hold_select(ipod_menu_t *m) {
  ipod_view_t *v = ipod_menu_top(m);
  if (v == NULL || v->nrows == 0 || v->sel < 0 || v->sel >= (int)v->nrows)
    return 0;
  const ipod_row_t row = v->rows[v->sel];
  uint32_t *tracks = NULL;
  size_t n = 0;
  switch ((ipod_row_kind_t)row.kind) {
    case IPOD_ROW_TRACK:
      if (v->is_otg)
        return 0; // already on it
      return otg_append(m, &v->tracks[row.value], 1);
    case IPOD_ROW_GROUP: {
      ipod_filter_t f = group_filter(m, v, &row);
      n = ipod_lib_query_tracks(m->lib, &f, IPOD_ORDER_ALBUM, &tracks);
      break;
    }
    case IPOD_ROW_ALL:
      n = ipod_lib_query_tracks(m->lib, &v->filter, IPOD_ORDER_ALBUM, &tracks);
      break;
    case IPOD_ROW_PLAYLIST:
      if (row.sub == IPOD_PL_ON_THE_GO)
        return 0;
      n = playlist_tracks(m, &row, &tracks);
      break;
    default:
      return 0;
  }
  n = otg_append(m, tracks, n);
  free(tracks);
  return n;
}

// ---------------------------------------------------------------------------
// Row text

const char *ipod_shuffle_name(ipod_shuffle_t s) {
  static const char *const NAMES[] = {"Off", "Songs", "Albums"};
  return s < IPOD_SHUFFLE_COUNT ? NAMES[s] : NAMES[0];
}

const char *ipod_repeat_name(ipod_repeat_t r) {
  static const char *const NAMES[] = {"Off", "One", "All"};
  return r < IPOD_REPEAT_COUNT ? NAMES[r] : NAMES[0];
}

static void info_text(const ipod_menu_t *m, ipod_info_id_t id, ipod_row_text_t *out) {
  uint32_t *tmp = NULL;
  uint64_t ms = 0;
  uint64_t bytes = 0;
  switch (id) {
    case IPOD_INFO_SONGS:
      ipod_text_copy(out->text, sizeof(out->text), "Songs");
      snprintf(out->right, sizeof(out->right), "%u", (unsigned)ipod_lib_count(m->lib));
      break;
    case IPOD_INFO_ALBUMS:
      ipod_text_copy(out->text, sizeof(out->text), "Albums");
      snprintf(out->right,
               sizeof(out->right),
               "%u",
               (unsigned)ipod_lib_query_groups(m->lib, IPOD_FIELD_ALBUM, NULL, &tmp));
      break;
    case IPOD_INFO_ARTISTS:
      ipod_text_copy(out->text, sizeof(out->text), "Artists");
      snprintf(out->right,
               sizeof(out->right),
               "%u",
               (unsigned)ipod_lib_query_groups(m->lib, IPOD_FIELD_ARTIST, NULL, &tmp));
      break;
    case IPOD_INFO_GENRES:
      ipod_text_copy(out->text, sizeof(out->text), "Genres");
      snprintf(out->right,
               sizeof(out->right),
               "%u",
               (unsigned)ipod_lib_query_groups(m->lib, IPOD_FIELD_GENRE, NULL, &tmp));
      break;
    case IPOD_INFO_PLAYLISTS:
      ipod_text_copy(out->text, sizeof(out->text), "Playlists");
      snprintf(out->right, sizeof(out->right), "%u", (unsigned)ipod_lib_playlist_count(m->lib));
      break;
    case IPOD_INFO_PLAYTIME:
      ipod_lib_totals(m->lib, &ms, NULL);
      ipod_text_copy(out->text, sizeof(out->text), "Play Time");
      snprintf(out->right,
               sizeof(out->right),
               "%u:%02u h",
               (unsigned)(ms / MS_PER_MIN / MIN_PER_HOUR),
               (unsigned)(ms / MS_PER_MIN % MIN_PER_HOUR));
      break;
    case IPOD_INFO_SIZE:
      ipod_lib_totals(m->lib, NULL, &bytes);
      ipod_text_copy(out->text, sizeof(out->text), "Music Size");
      snprintf(out->right, sizeof(out->right), "%.1f MB", (double)bytes / BYTES_PER_MB);
      break;
    case IPOD_INFO_FORMAT:
      ipod_text_copy(out->text, sizeof(out->text), "Format");
      ipod_text_copy(out->right, sizeof(out->right), "FLAC");
      break;
    case IPOD_INFO_VERSION:
      ipod_text_copy(out->text, sizeof(out->text), "Version");
      ipod_text_copy(out->right, sizeof(out->right), IPOD_APP_VERSION);
      break;
    default:
      break;
  }
  free(tmp);
}

void ipod_menu_row_text(const ipod_menu_t *m,
                        const ipod_view_t *v,
                        size_t row,
                        uint32_t playing_track,
                        ipod_row_text_t *out) {
  memset(out, 0, sizeof(*out));
  out->is_selectable = true;
  if (v == NULL || row >= v->nrows)
    return;
  const ipod_row_t *r = &v->rows[row];
  switch ((ipod_row_kind_t)r->kind) {
    case IPOD_ROW_NAV:
      ipod_text_copy(out->text, sizeof(out->text), view_name((ipod_view_kind_t)r->sub, r->value));
      out->icon = IPOD_ICON_ARROW;
      break;
    case IPOD_ROW_GROUP:
      ipod_text_copy(
          out->text,
          sizeof(out->text),
          ipod_lib_disp(m->lib, ipod_lib_field_value(m->lib, (ipod_field_t)r->sub, r->value)));
      out->icon = IPOD_ICON_ARROW;
      break;
    case IPOD_ROW_ALL:
      ipod_text_copy(out->text, sizeof(out->text), "All Songs");
      out->icon = IPOD_ICON_ARROW;
      break;
    case IPOD_ROW_TRACK: {
      const uint32_t track = r->value < v->ntracks ? v->tracks[r->value] : IPOD_NO_TRACK;
      const ipod_track_t *t = ipod_lib_track(m->lib, track);
      ipod_text_copy(
          out->text, sizeof(out->text), t != NULL ? ipod_lib_disp(m->lib, t->title) : "");
      if (track == playing_track)
        out->icon = IPOD_ICON_SPEAKER;
      break;
    }
    case IPOD_ROW_PLAYLIST:
      ipod_text_copy(out->text, sizeof(out->text), playlist_name(m, r));
      out->icon = IPOD_ICON_ARROW;
      break;
    case IPOD_ROW_ACTION: {
      static const char *const NAMES[] = {
          "Shuffle Songs", "Update Library", "Clear Playlist", "Cancel", "Reset"};
      if (r->sub < sizeof(NAMES) / sizeof(NAMES[0]))
        ipod_text_copy(out->text, sizeof(out->text), NAMES[r->sub]);
      break;
    }
    case IPOD_ROW_SETTING: {
      const ipod_settings_t *s = m->settings;
      switch ((ipod_setting_id_t)r->sub) {
        case IPOD_SETTING_SHUFFLE:
          ipod_text_copy(out->text, sizeof(out->text), "Shuffle");
          ipod_text_copy(out->right, sizeof(out->right), ipod_shuffle_name(s->shuffle));
          break;
        case IPOD_SETTING_REPEAT:
          ipod_text_copy(out->text, sizeof(out->text), "Repeat");
          ipod_text_copy(out->right, sizeof(out->right), ipod_repeat_name(s->repeat));
          break;
        case IPOD_SETTING_SOUND_CHECK:
          ipod_text_copy(out->text, sizeof(out->text), "Sound Check");
          ipod_text_copy(out->right, sizeof(out->right), s->sound_check ? "On" : "Off");
          break;
        case IPOD_SETTING_CLICKER:
          ipod_text_copy(out->text, sizeof(out->text), "Clicker");
          ipod_text_copy(out->right, sizeof(out->right), s->clicker ? "On" : "Off");
          break;
        default:
          break;
      }
      break;
    }
    case IPOD_ROW_CHOICE:
      if (r->sub == IPOD_SETTING_EQ) {
        ipod_text_copy(out->text, sizeof(out->text), ipod_eq_name((int)r->value));
        if (m->settings != NULL && m->settings->eq_preset == (int)r->value)
          out->icon = IPOD_ICON_CHECK;
      } else {
        if (r->value >= IPOD_VOLUME_MAX)
          ipod_text_copy(out->text, sizeof(out->text), "No Limit");
        else
          snprintf(out->text, sizeof(out->text), "%u%%", (unsigned)r->value);
        if (m->settings != NULL && m->settings->volume_limit == r->value)
          out->icon = IPOD_ICON_CHECK;
      }
      break;
    case IPOD_ROW_INFO:
      info_text(m, (ipod_info_id_t)r->sub, out);
      out->is_selectable = false;
      break;
    case IPOD_ROW_EMPTY:
      ipod_text_copy(out->text, sizeof(out->text), v->is_otg ? "No Songs" : "No Music");
      out->is_selectable = false;
      break;
    default:
      break;
  }
}

char ipod_menu_row_letter(const ipod_menu_t *m, const ipod_view_t *v, size_t row) {
  if (v == NULL || !v->has_letters || row >= v->nrows)
    return 0;
  const ipod_row_t *r = &v->rows[row];
  if (r->kind == IPOD_ROW_TRACK && r->value < v->ntracks) {
    const ipod_track_t *t = ipod_lib_track(m->lib, v->tracks[r->value]);
    return t != NULL ? ipod_text_index_char(ipod_lib_key(m->lib, t->title)) : 0;
  }
  if (r->kind == IPOD_ROW_GROUP)
    return ipod_text_index_char(
        ipod_lib_key(m->lib, ipod_lib_field_value(m->lib, (ipod_field_t)r->sub, r->value)));
  return 0;
}

uint32_t *ipod_menu_album_groups(const ipod_menu_t *m, const uint32_t *tracks, size_t count) {
  if (count == 0)
    return NULL;
  uint32_t *g = malloc(count * sizeof(uint32_t));
  if (g == NULL) {
    ESP_LOGE(TAG, "album groups alloc failed");
    return NULL;
  }
  for (size_t i = 0; i < count; i++) {
    const ipod_track_t *t = ipod_lib_track(m->lib, tracks[i]);
    g[i] = t != NULL ? t->album : 0;
  }
  return g;
}
