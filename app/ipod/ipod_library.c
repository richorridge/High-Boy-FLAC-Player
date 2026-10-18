// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "ipod_meta.h"
#include "ipod_text.h"

#define POOL_INITIAL       (16u * 1024u)
#define STRS_INITIAL       256u
#define TRACKS_INITIAL     128u
#define BUCKETS_INITIAL    256u
#define PLAYLISTS_INITIAL  8u
#define TEXT_BUF_LEN       256
#define PATH_BUF_LEN       512
#define LINE_BUF_LEN       1024
#define SCAN_MAX_DEPTH     12
#define FNV_OFFSET         2166136261u
#define FNV_PRIME          16777619u
#define CACHE_MAGIC        0x42445049u // "IPDB" little-endian
#define CACHE_TRAILER      0x21444E45u // "END!"
#define CACHE_VERSION      1u
#define CACHE_MAX_TRACKS   200000u
#define CACHE_MAX_STR      1023u
#define RECENT_ADDED_MAX   50u
#define TOP_PLAYED_MAX     25u
#define RECENT_PLAYED_MAX  25u
#define TOP_RATED_MIN      4u
#define CDB_PER_DB         100.0f
#define EXT_FLAC           ".flac"
#define EXT_M3U            ".m3u"
#define EXT_M3U8           ".m3u8"
#define UTF8_BOM           "\xEF\xBB\xBF"

static const char *TAG = "IPOD_LIB";

// Folders TentacleOS keeps on the card root; never scanned for music.
static const char *const SKIP_DIRS[] = {
    "assets", "config", "nfc", "rfid", "subghz", "ir", "wifi", "ble", "lora", "badusb",
    "ota", "System Volume Information", "LOST.DIR", "$RECYCLE.BIN",
};

typedef struct {
  uint32_t raw;  ///< pool offset of the UTF-8 text
  uint32_t disp; ///< pool offset of the folded text
  uint32_t key;  ///< pool offset of the sort key
  uint32_t hash;
  uint32_t next; ///< next entry in the bucket chain, +1 (0 = end)
} str_entry_t;

struct ipod_library {
  char *pool;
  size_t pool_len;
  size_t pool_cap;
  str_entry_t *strs;
  uint32_t nstr;
  uint32_t str_cap;
  uint32_t *buckets; ///< head entry index +1 (0 = empty)
  uint32_t nbuckets;
  ipod_track_t *tracks;
  uint32_t ntracks;
  uint32_t track_cap;
  uint32_t *path_slots; ///< open addressing: track index +1 (0 = empty)
  uint32_t path_cap;
  ipod_str_t *pl_paths;
  ipod_str_t *pl_names;
  uint32_t nplaylists;
  uint32_t pl_cap;
  bool is_oom;
};

typedef struct {
  const char *path;
  const char *title;
  const char *artist;
  const char *album;
  const char *album_artist;
  const char *genre;
  const char *composer;
  ipod_track_t num; ///< numeric fields; string ids are ignored
} track_rec_t;

typedef int (*idx_cmp_fn)(uint32_t a, uint32_t b, const void *ctx);

typedef struct {
  const ipod_library_t *lib;
  ipod_field_t field;
} group_ctx_t;

typedef struct {
  ipod_library_t *lib;
  const ipod_library_t *prev;
  ipod_scan_progress_t *progress;
} scan_ctx_t;

static uint32_t hash_bytes(const char *s, bool fold_case);
static ipod_str_t intern(ipod_library_t *lib, const char *s, bool is_text);
static bool add_track(ipod_library_t *lib, const track_rec_t *rec);
static bool add_playlist(ipod_library_t *lib, const char *path);
static bool build_path_index(ipod_library_t *lib);
static void sort_idx(uint32_t *arr, size_t n, idx_cmp_fn cmp, const void *ctx);
static bool track_matches(const ipod_track_t *t, const ipod_filter_t *f);
static int cmp_title(uint32_t a, uint32_t b, const void *ctx);
static int cmp_album_order(uint32_t a, uint32_t b, const void *ctx);
static int cmp_group(uint32_t a, uint32_t b, const void *ctx);
static bool same_group(const ipod_library_t *lib, ipod_field_t field, uint32_t a, uint32_t b);
static void scan_dir(scan_ctx_t *ctx, char *path, size_t len, int depth);
static bool has_ext(const char *name, const char *ext);
static void normalize_path(char *path);
static void sort_playlists(ipod_library_t *lib);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ipod_library_t *ipod_lib_create(void) {
  ipod_library_t *lib = calloc(1, sizeof(*lib));
  if (lib == NULL) {
    ESP_LOGE(TAG, "library alloc failed");
    return NULL;
  }
  lib->pool = malloc(POOL_INITIAL);
  lib->strs = malloc(STRS_INITIAL * sizeof(str_entry_t));
  lib->buckets = calloc(BUCKETS_INITIAL, sizeof(uint32_t));
  if (lib->pool == NULL || lib->strs == NULL || lib->buckets == NULL) {
    ESP_LOGE(TAG, "library tables alloc failed");
    ipod_lib_destroy(lib);
    return NULL;
  }
  lib->pool_cap = POOL_INITIAL;
  lib->str_cap = STRS_INITIAL;
  lib->nbuckets = BUCKETS_INITIAL;
  // Entry 0 is the empty string so that IPOD_STR_EMPTY needs no special case.
  lib->pool[0] = '\0';
  lib->pool_len = 1;
  lib->strs[0] = (str_entry_t){0};
  lib->nstr = 1;
  return lib;
}

void ipod_lib_destroy(ipod_library_t *lib) {
  if (lib == NULL)
    return;
  free(lib->pool);
  free(lib->strs);
  free(lib->buckets);
  free(lib->tracks);
  free(lib->path_slots);
  free(lib->pl_paths);
  free(lib->pl_names);
  free(lib);
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

ipod_library_t *
ipod_lib_scan(const char *root, const ipod_library_t *previous, ipod_scan_progress_t *progress) {
  ipod_scan_progress_t local = {0};
  if (progress == NULL)
    progress = &local;
  ipod_library_t *lib = ipod_lib_create();
  if (lib == NULL)
    return NULL;

  char path[PATH_BUF_LEN];
  ipod_text_copy(path, sizeof(path), root);
  size_t len = strlen(path);
  while (len > 1 && path[len - 1] == '/')
    path[--len] = '\0';

  scan_ctx_t ctx = {.lib = lib, .prev = previous, .progress = progress};
  scan_dir(&ctx, path, len, 0);
  sort_playlists(lib);

  const bool cancelled = atomic_load(&progress->cancel);
  if (cancelled || lib->is_oom || !build_path_index(lib)) {
    ESP_LOGW(TAG, "scan aborted (cancel=%d oom=%d)", (int)cancelled, (int)lib->is_oom);
    ipod_lib_destroy(lib);
    return NULL;
  }
  ESP_LOGI(TAG,
           "scan of %s: %u tracks, %u playlists (%u parsed)",
           root,
           (unsigned)lib->ntracks,
           (unsigned)lib->nplaylists,
           (unsigned)atomic_load(&progress->files_parsed));
  return lib;
}

static void rec_from_track(const ipod_library_t *lib,
                           const ipod_track_t *t,
                           bool with_stats,
                           track_rec_t *rec) {
  rec->path = ipod_lib_str(lib, t->path);
  rec->title = ipod_lib_str(lib, t->title);
  rec->artist = ipod_lib_str(lib, t->artist);
  rec->album = ipod_lib_str(lib, t->album);
  rec->album_artist = ipod_lib_str(lib, t->album_artist);
  rec->genre = ipod_lib_str(lib, t->genre);
  rec->composer = ipod_lib_str(lib, t->composer);
  // Field by field: the statistics may be written concurrently by the player
  // when the caller asked us not to touch them.
  memset(&rec->num, 0, sizeof(rec->num));
  rec->num.duration_ms = t->duration_ms;
  rec->num.file_size = t->file_size;
  rec->num.mtime = t->mtime;
  rec->num.picture_offset = t->picture_offset;
  rec->num.picture_size = t->picture_size;
  rec->num.sample_rate = t->sample_rate;
  rec->num.track_no = t->track_no;
  rec->num.disc_no = t->disc_no;
  rec->num.year = t->year;
  rec->num.rg_track_cdb = t->rg_track_cdb;
  rec->num.rg_album_cdb = t->rg_album_cdb;
  rec->num.channels = t->channels;
  rec->num.bits_per_sample = t->bits_per_sample;
  rec->num.flags = t->flags;
  if (with_stats) {
    rec->num.rating = t->rating;
    rec->num.play_count = t->play_count;
    rec->num.last_played = t->last_played;
  }
}

static void scan_file(scan_ctx_t *ctx, const char *path, const struct stat *st) {
  atomic_fetch_add(&ctx->progress->files_found, 1);
  const uint32_t size = (uint32_t)st->st_size;
  const uint32_t mtime = (uint32_t)st->st_mtime;
  uint32_t prev_idx = 0;
  if (ctx->prev != NULL && ipod_lib_find_path(ctx->prev, path, &prev_idx)) {
    const ipod_track_t *old = ipod_lib_track(ctx->prev, prev_idx);
    track_rec_t rec;
    rec_from_track(ctx->prev, old, !ctx->progress->skip_stats, &rec);
    rec.path = path;
    if (old->file_size == size && old->mtime == mtime) {
      add_track(ctx->lib, &rec);
      return;
    }
  }

  ipod_meta_t *meta = malloc(sizeof(*meta));
  if (meta == NULL) {
    ESP_LOGE(TAG, "meta alloc failed");
    ctx->lib->is_oom = true;
    return;
  }
  atomic_fetch_add(&ctx->progress->files_parsed, 1);
  if (!ipod_meta_read_flac(path, meta)) {
    ESP_LOGW(TAG, "not a valid FLAC: %s", path);
    free(meta);
    return;
  }
  track_rec_t rec = {
      .path = path,
      .title = meta->title,
      .artist = meta->artist,
      .album = meta->album,
      .album_artist = meta->album_artist,
      .genre = meta->genre,
      .composer = meta->composer,
  };
  rec.num.duration_ms = ipod_meta_duration_ms(meta);
  rec.num.file_size = size;
  rec.num.mtime = mtime;
  rec.num.picture_offset = meta->picture_offset;
  rec.num.picture_size = meta->picture_size;
  rec.num.sample_rate = meta->sample_rate;
  rec.num.track_no = meta->track_no;
  rec.num.disc_no = meta->disc_no;
  rec.num.year = meta->year;
  rec.num.rg_track_cdb =
      meta->has_rg_track ? (int16_t)(meta->rg_track_gain_db * CDB_PER_DB) : IPOD_RG_NONE;
  rec.num.rg_album_cdb =
      meta->has_rg_album ? (int16_t)(meta->rg_album_gain_db * CDB_PER_DB) : IPOD_RG_NONE;
  rec.num.channels = meta->channels;
  rec.num.bits_per_sample = meta->bits_per_sample;
  rec.num.flags = meta->is_compilation ? IPOD_TRACK_COMPILATION : 0;
  // A re-tagged file keeps its listening statistics.
  if (ctx->prev != NULL && !ctx->progress->skip_stats &&
      ipod_lib_find_path(ctx->prev, path, &prev_idx)) {
    const ipod_track_t *old = ipod_lib_track(ctx->prev, prev_idx);
    rec.num.rating = old->rating;
    rec.num.play_count = old->play_count;
    rec.num.last_played = old->last_played;
  }
  add_track(ctx->lib, &rec);
  free(meta);
}

static bool is_skipped_dir(const char *name, int depth) {
  if (name[0] == '.')
    return true;
  if (depth != 0)
    return false;
  for (size_t i = 0; i < sizeof(SKIP_DIRS) / sizeof(SKIP_DIRS[0]); i++) {
    if (ipod_text_equal_ci(name, SKIP_DIRS[i]))
      return true;
  }
  return false;
}

static void scan_dir(scan_ctx_t *ctx, char *path, size_t len, int depth) {
  DIR *dir = opendir(path);
  if (dir == NULL)
    return;
  struct dirent *de;
  while ((de = readdir(dir)) != NULL && !atomic_load(&ctx->progress->cancel) &&
         !ctx->lib->is_oom) {
    const char *name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;
    size_t nlen = strlen(name);
    if (len + 1 + nlen >= PATH_BUF_LEN)
      continue;
    path[len] = '/';
    memcpy(path + len + 1, name, nlen + 1);
    struct stat st;
    if (stat(path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        if (depth < SCAN_MAX_DEPTH && !is_skipped_dir(name, depth))
          scan_dir(ctx, path, len + 1 + nlen, depth + 1);
      } else if (S_ISREG(st.st_mode) && name[0] != '.') {
        if (has_ext(name, EXT_FLAC))
          scan_file(ctx, path, &st);
        else if (has_ext(name, EXT_M3U) || has_ext(name, EXT_M3U8))
          add_playlist(ctx->lib, path);
      }
    }
    path[len] = '\0';
  }
  closedir(dir);
}

bool ipod_lib_same_files(const ipod_library_t *a, const ipod_library_t *b) {
  if (a == NULL || b == NULL || a->ntracks != b->ntracks || a->nplaylists != b->nplaylists)
    return false;
  for (uint32_t i = 0; i < a->ntracks; i++) {
    const ipod_track_t *ta = &a->tracks[i];
    uint32_t j = 0;
    if (!ipod_lib_find_path(b, ipod_lib_str(a, ta->path), &j))
      return false;
    if (b->tracks[j].file_size != ta->file_size || b->tracks[j].mtime != ta->mtime)
      return false;
  }
  for (uint32_t i = 0; i < a->nplaylists; i++) {
    if (strcmp(ipod_lib_str(a, a->pl_paths[i]), ipod_lib_str(b, b->pl_paths[i])) != 0)
      return false;
  }
  return true;
}

void ipod_lib_copy_stats(ipod_library_t *dst, const ipod_library_t *src) {
  if (dst == NULL || src == NULL)
    return;
  for (uint32_t i = 0; i < dst->ntracks; i++) {
    uint32_t j = 0;
    if (!ipod_lib_find_path(src, ipod_lib_str(dst, dst->tracks[i].path), &j))
      continue;
    dst->tracks[i].rating = src->tracks[j].rating;
    dst->tracks[i].play_count = src->tracks[j].play_count;
    dst->tracks[i].last_played = src->tracks[j].last_played;
  }
}

// ---------------------------------------------------------------------------
// Cache file
// ---------------------------------------------------------------------------

static bool put_u32(FILE *f, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool put_u16(FILE *f, uint16_t v) {
  uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
  return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool put_u8(FILE *f, uint8_t v) {
  return fputc(v, f) != EOF;
}

static bool put_str(FILE *f, const char *s) {
  size_t len = strlen(s);
  if (len > CACHE_MAX_STR)
    len = CACHE_MAX_STR;
  return put_u16(f, (uint16_t)len) && fwrite(s, 1, len, f) == len;
}

static bool get_u32(FILE *f, uint32_t *v) {
  uint8_t b[4];
  if (fread(b, 1, sizeof(b), f) != sizeof(b))
    return false;
  *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
  return true;
}

static bool get_u16(FILE *f, uint16_t *v) {
  uint8_t b[2];
  if (fread(b, 1, sizeof(b), f) != sizeof(b))
    return false;
  *v = (uint16_t)(b[0] | (b[1] << 8));
  return true;
}

static bool get_u8(FILE *f, uint8_t *v) {
  int c = fgetc(f);
  if (c == EOF)
    return false;
  *v = (uint8_t)c;
  return true;
}

static bool get_str(FILE *f, char *buf) {
  uint16_t len = 0;
  if (!get_u16(f, &len) || len > CACHE_MAX_STR)
    return false;
  if (fread(buf, 1, len, f) != len)
    return false;
  buf[len] = '\0';
  return true;
}

bool ipod_lib_save(const ipod_library_t *lib, const char *path) {
  if (lib == NULL || path == NULL)
    return false;
  char tmp[PATH_BUF_LEN];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (f == NULL) {
    ESP_LOGW(TAG, "cannot write %s", tmp);
    return false;
  }
  bool ok = put_u32(f, CACHE_MAGIC) && put_u32(f, CACHE_VERSION) && put_u32(f, lib->ntracks) &&
            put_u32(f, lib->nplaylists);
  for (uint32_t i = 0; ok && i < lib->ntracks; i++) {
    const ipod_track_t *t = &lib->tracks[i];
    ok = put_u32(f, t->duration_ms) && put_u32(f, t->file_size) && put_u32(f, t->mtime) &&
         put_u32(f, t->picture_offset) && put_u32(f, t->picture_size) &&
         put_u32(f, t->sample_rate) && put_u32(f, t->last_played) && put_u16(f, t->track_no) &&
         put_u16(f, t->disc_no) && put_u16(f, t->year) && put_u16(f, t->play_count) &&
         put_u16(f, (uint16_t)t->rg_track_cdb) && put_u16(f, (uint16_t)t->rg_album_cdb) &&
         put_u8(f, t->channels) && put_u8(f, t->bits_per_sample) && put_u8(f, t->flags) &&
         put_u8(f, t->rating) && put_str(f, ipod_lib_str(lib, t->path)) &&
         put_str(f, ipod_lib_str(lib, t->title)) && put_str(f, ipod_lib_str(lib, t->artist)) &&
         put_str(f, ipod_lib_str(lib, t->album)) &&
         put_str(f, ipod_lib_str(lib, t->album_artist)) &&
         put_str(f, ipod_lib_str(lib, t->genre)) && put_str(f, ipod_lib_str(lib, t->composer));
  }
  for (uint32_t i = 0; ok && i < lib->nplaylists; i++)
    ok = put_str(f, ipod_lib_str(lib, lib->pl_paths[i]));
  ok = ok && put_u32(f, CACHE_TRAILER);
  if (fclose(f) != 0)
    ok = false;
  if (!ok) {
    remove(tmp);
    return false;
  }
  remove(path); // FAT cannot rename over an existing file
  if (rename(tmp, path) != 0) {
    ESP_LOGW(TAG, "rename %s failed", tmp);
    return false;
  }
  return true;
}

ipod_library_t *ipod_lib_load(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return NULL;
  ipod_library_t *lib = ipod_lib_create();
  char *bufs = malloc(7 * (CACHE_MAX_STR + 1));
  bool ok = lib != NULL && bufs != NULL;
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t ntracks = 0;
  uint32_t nplaylists = 0;
  ok = ok && get_u32(f, &magic) && get_u32(f, &version) && get_u32(f, &ntracks) &&
       get_u32(f, &nplaylists) && magic == CACHE_MAGIC && version == CACHE_VERSION &&
       ntracks <= CACHE_MAX_TRACKS && nplaylists <= CACHE_MAX_TRACKS;
  for (uint32_t i = 0; ok && i < ntracks; i++) {
    track_rec_t rec = {0};
    ipod_track_t *t = &rec.num;
    uint16_t rg_t = 0;
    uint16_t rg_a = 0;
    char *s[7];
    for (int k = 0; k < 7; k++)
      s[k] = bufs + k * (CACHE_MAX_STR + 1);
    ok = get_u32(f, &t->duration_ms) && get_u32(f, &t->file_size) && get_u32(f, &t->mtime) &&
         get_u32(f, &t->picture_offset) && get_u32(f, &t->picture_size) &&
         get_u32(f, &t->sample_rate) && get_u32(f, &t->last_played) && get_u16(f, &t->track_no) &&
         get_u16(f, &t->disc_no) && get_u16(f, &t->year) && get_u16(f, &t->play_count) &&
         get_u16(f, &rg_t) && get_u16(f, &rg_a) && get_u8(f, &t->channels) &&
         get_u8(f, &t->bits_per_sample) && get_u8(f, &t->flags) && get_u8(f, &t->rating) &&
         get_str(f, s[0]) && get_str(f, s[1]) && get_str(f, s[2]) && get_str(f, s[3]) &&
         get_str(f, s[4]) && get_str(f, s[5]) && get_str(f, s[6]);
    if (!ok)
      break;
    t->rg_track_cdb = (int16_t)rg_t;
    t->rg_album_cdb = (int16_t)rg_a;
    if (t->rating > IPOD_RATING_MAX)
      t->rating = IPOD_RATING_MAX;
    rec.path = s[0];
    rec.title = s[1];
    rec.artist = s[2];
    rec.album = s[3];
    rec.album_artist = s[4];
    rec.genre = s[5];
    rec.composer = s[6];
    ok = add_track(lib, &rec);
  }
  for (uint32_t i = 0; ok && i < nplaylists; i++)
    ok = get_str(f, bufs) && add_playlist(lib, bufs);
  uint32_t trailer = 0;
  ok = ok && get_u32(f, &trailer) && trailer == CACHE_TRAILER && build_path_index(lib);
  fclose(f);
  free(bufs);
  if (!ok) {
    ESP_LOGW(TAG, "cache %s missing or corrupt", path);
    ipod_lib_destroy(lib);
    return NULL;
  }
  sort_playlists(lib);
  return lib;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

size_t ipod_lib_count(const ipod_library_t *lib) {
  return lib != NULL ? lib->ntracks : 0;
}

const ipod_track_t *ipod_lib_track(const ipod_library_t *lib, uint32_t index) {
  if (lib == NULL || index >= lib->ntracks)
    return NULL;
  return &lib->tracks[index];
}

ipod_track_t *ipod_lib_track_mut(ipod_library_t *lib, uint32_t index) {
  if (lib == NULL || index >= lib->ntracks)
    return NULL;
  return &lib->tracks[index];
}

const char *ipod_lib_str(const ipod_library_t *lib, ipod_str_t id) {
  if (lib == NULL || id >= lib->nstr)
    return "";
  return lib->pool + lib->strs[id].raw;
}

const char *ipod_lib_disp(const ipod_library_t *lib, ipod_str_t id) {
  if (lib == NULL || id >= lib->nstr)
    return "";
  return lib->pool + lib->strs[id].disp;
}

const char *ipod_lib_key(const ipod_library_t *lib, ipod_str_t id) {
  if (lib == NULL || id >= lib->nstr)
    return "";
  return lib->pool + lib->strs[id].key;
}

bool ipod_lib_find_path(const ipod_library_t *lib, const char *path, uint32_t *out_index) {
  if (lib == NULL || path == NULL || lib->path_cap == 0)
    return false;
  uint32_t mask = lib->path_cap - 1;
  for (uint32_t h = hash_bytes(path, true) & mask, probes = 0; probes < lib->path_cap;
       h = (h + 1) & mask, probes++) {
    uint32_t slot = lib->path_slots[h];
    if (slot == 0)
      return false;
    if (ipod_text_equal_ci(ipod_lib_str(lib, lib->tracks[slot - 1].path), path)) {
      if (out_index != NULL)
        *out_index = slot - 1;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

size_t ipod_lib_query_tracks(const ipod_library_t *lib,
                             const ipod_filter_t *filter,
                             ipod_order_t order,
                             uint32_t **out) {
  *out = NULL;
  if (lib == NULL || lib->ntracks == 0)
    return 0;
  uint32_t *arr = malloc(lib->ntracks * sizeof(uint32_t));
  if (arr == NULL) {
    ESP_LOGE(TAG, "query alloc failed");
    return 0;
  }
  size_t n = 0;
  for (uint32_t i = 0; i < lib->ntracks; i++) {
    if (filter == NULL || track_matches(&lib->tracks[i], filter))
      arr[n++] = i;
  }
  if (n == 0) {
    free(arr);
    return 0;
  }
  sort_idx(arr, n, order == IPOD_ORDER_ALBUM ? cmp_album_order : cmp_title, lib);
  *out = arr;
  return n;
}

size_t ipod_lib_query_groups(const ipod_library_t *lib,
                             ipod_field_t field,
                             const ipod_filter_t *filter,
                             uint32_t **out) {
  *out = NULL;
  if (lib == NULL || lib->ntracks == 0 || field >= IPOD_FIELD_COUNT)
    return 0;
  uint32_t *arr = malloc(lib->ntracks * sizeof(uint32_t));
  if (arr == NULL) {
    ESP_LOGE(TAG, "group alloc failed");
    return 0;
  }
  size_t n = 0;
  for (uint32_t i = 0; i < lib->ntracks; i++) {
    const ipod_track_t *t = &lib->tracks[i];
    if (filter != NULL && !track_matches(t, filter))
      continue;
    if (ipod_lib_field_value(lib, field, i) == IPOD_STR_EMPTY)
      continue;
    arr[n++] = i;
  }
  group_ctx_t ctx = {.lib = lib, .field = field};
  sort_idx(arr, n, cmp_group, &ctx);
  size_t w = 0;
  for (size_t r = 0; r < n; r++) {
    if (w == 0 || !same_group(lib, field, arr[w - 1], arr[r]))
      arr[w++] = arr[r];
  }
  if (w == 0) {
    free(arr);
    return 0;
  }
  *out = arr;
  return w;
}

ipod_str_t ipod_lib_field_value(const ipod_library_t *lib, ipod_field_t field, uint32_t track) {
  const ipod_track_t *t = ipod_lib_track(lib, track);
  if (t == NULL)
    return IPOD_STR_EMPTY;
  switch (field) {
    case IPOD_FIELD_ARTIST:
      return t->album_artist;
    case IPOD_FIELD_ALBUM:
      return t->album;
    case IPOD_FIELD_GENRE:
      return t->genre;
    case IPOD_FIELD_COMPOSER:
      return t->composer;
    default:
      return IPOD_STR_EMPTY;
  }
}

size_t ipod_lib_compilation_count(const ipod_library_t *lib) {
  size_t n = 0;
  for (uint32_t i = 0; lib != NULL && i < lib->ntracks; i++) {
    if (lib->tracks[i].flags & IPOD_TRACK_COMPILATION)
      n++;
  }
  return n;
}

static int cmp_mtime_desc(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  uint32_t ma = lib->tracks[a].mtime;
  uint32_t mb = lib->tracks[b].mtime;
  if (ma != mb)
    return ma > mb ? -1 : 1;
  return cmp_album_order(a, b, ctx);
}

static int cmp_plays_desc(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  uint16_t pa = lib->tracks[a].play_count;
  uint16_t pb = lib->tracks[b].play_count;
  if (pa != pb)
    return pa > pb ? -1 : 1;
  return cmp_title(a, b, ctx);
}

static int cmp_last_played_desc(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  uint32_t la = lib->tracks[a].last_played;
  uint32_t lb = lib->tracks[b].last_played;
  if (la != lb)
    return la > lb ? -1 : 1;
  return cmp_title(a, b, ctx);
}

static int cmp_rating_desc(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  uint8_t ra = lib->tracks[a].rating;
  uint8_t rb = lib->tracks[b].rating;
  if (ra != rb)
    return ra > rb ? -1 : 1;
  return cmp_title(a, b, ctx);
}

size_t ipod_lib_smart(const ipod_library_t *lib, ipod_smart_t kind, uint32_t **out) {
  *out = NULL;
  if (lib == NULL || lib->ntracks == 0)
    return 0;
  uint32_t *arr = malloc(lib->ntracks * sizeof(uint32_t));
  if (arr == NULL) {
    ESP_LOGE(TAG, "smart alloc failed");
    return 0;
  }
  size_t n = 0;
  size_t limit = lib->ntracks;
  idx_cmp_fn cmp = cmp_title;
  for (uint32_t i = 0; i < lib->ntracks; i++) {
    const ipod_track_t *t = &lib->tracks[i];
    bool take = false;
    switch (kind) {
      case IPOD_SMART_RECENTLY_ADDED:
        take = true;
        break;
      case IPOD_SMART_TOP_PLAYED:
        take = t->play_count > 0;
        break;
      case IPOD_SMART_RECENTLY_PLAYED:
        take = t->last_played > 0;
        break;
      case IPOD_SMART_TOP_RATED:
        take = t->rating >= TOP_RATED_MIN;
        break;
      default:
        break;
    }
    if (take)
      arr[n++] = i;
  }
  switch (kind) {
    case IPOD_SMART_RECENTLY_ADDED:
      cmp = cmp_mtime_desc;
      limit = RECENT_ADDED_MAX;
      break;
    case IPOD_SMART_TOP_PLAYED:
      cmp = cmp_plays_desc;
      limit = TOP_PLAYED_MAX;
      break;
    case IPOD_SMART_RECENTLY_PLAYED:
      cmp = cmp_last_played_desc;
      limit = RECENT_PLAYED_MAX;
      break;
    case IPOD_SMART_TOP_RATED:
      cmp = cmp_rating_desc;
      break;
    default:
      break;
  }
  sort_idx(arr, n, cmp, lib);
  if (n > limit)
    n = limit;
  if (n == 0) {
    free(arr);
    return 0;
  }
  *out = arr;
  return n;
}

size_t ipod_lib_search(const ipod_library_t *lib, const char *needle, size_t max, ipod_hit_t **out) {
  *out = NULL;
  if (lib == NULL || lib->ntracks == 0 || needle == NULL || needle[0] == '\0' || max == 0)
    return 0;
  char folded[TEXT_BUF_LEN];
  ipod_text_fold(needle, folded, sizeof(folded));

  uint32_t *artists = malloc(lib->ntracks * sizeof(uint32_t));
  uint32_t *albums = malloc(lib->ntracks * sizeof(uint32_t));
  uint32_t *songs = malloc(lib->ntracks * sizeof(uint32_t));
  uint8_t *seen = calloc(lib->nstr, 1); // artist string ids already listed
  ipod_hit_t *hits = NULL;
  size_t na = 0;
  size_t nb = 0;
  size_t ns = 0;
  size_t total = 0;
  if (artists == NULL || albums == NULL || songs == NULL || seen == NULL) {
    ESP_LOGE(TAG, "search alloc failed");
    goto cleanup;
  }

  for (uint32_t i = 0; i < lib->ntracks; i++) {
    const ipod_track_t *t = &lib->tracks[i];
    if (!seen[t->album_artist] &&
        ipod_text_contains_ci(ipod_lib_disp(lib, t->album_artist), folded)) {
      seen[t->album_artist] = 1;
      artists[na++] = i;
    }
    if (ipod_text_contains_ci(ipod_lib_disp(lib, t->album), folded)) {
      bool dup = false;
      for (size_t k = 0; k < nb && !dup; k++)
        dup = same_group(lib, IPOD_FIELD_ALBUM, albums[k], i);
      if (!dup)
        albums[nb++] = i;
    }
    if (ipod_text_contains_ci(ipod_lib_disp(lib, t->title), folded))
      songs[ns++] = i;
  }
  group_ctx_t gart = {.lib = lib, .field = IPOD_FIELD_ARTIST};
  group_ctx_t galb = {.lib = lib, .field = IPOD_FIELD_ALBUM};
  sort_idx(artists, na, cmp_group, &gart);
  sort_idx(albums, nb, cmp_group, &galb);
  sort_idx(songs, ns, cmp_title, lib);

  total = na + nb + ns;
  if (total > max)
    total = max;
  if (total == 0)
    goto cleanup;
  hits = malloc(total * sizeof(ipod_hit_t));
  if (hits == NULL) {
    ESP_LOGE(TAG, "search hits alloc failed");
    total = 0;
    goto cleanup;
  }
  size_t w = 0;
  for (size_t k = 0; k < na && w < total; k++)
    hits[w++] = (ipod_hit_t){.kind = IPOD_HIT_ARTIST, .track = artists[k]};
  for (size_t k = 0; k < nb && w < total; k++)
    hits[w++] = (ipod_hit_t){.kind = IPOD_HIT_ALBUM, .track = albums[k]};
  for (size_t k = 0; k < ns && w < total; k++)
    hits[w++] = (ipod_hit_t){.kind = IPOD_HIT_SONG, .track = songs[k]};
  *out = hits;

cleanup:
  free(artists);
  free(albums);
  free(songs);
  free(seen);
  return total;
}

size_t ipod_lib_playlist_count(const ipod_library_t *lib) {
  return lib != NULL ? lib->nplaylists : 0;
}

const char *ipod_lib_playlist_name(const ipod_library_t *lib, size_t index) {
  if (lib == NULL || index >= lib->nplaylists)
    return "";
  return ipod_lib_disp(lib, lib->pl_names[index]);
}

size_t ipod_lib_playlist_tracks(const ipod_library_t *lib, size_t index, uint32_t **out) {
  *out = NULL;
  if (lib == NULL || index >= lib->nplaylists)
    return 0;
  return ipod_lib_read_m3u(lib, ipod_lib_str(lib, lib->pl_paths[index]), out);
}

size_t ipod_lib_read_m3u(const ipod_library_t *lib, const char *m3u_path, uint32_t **out) {
  *out = NULL;
  if (lib == NULL || m3u_path == NULL)
    return 0;
  FILE *f = fopen(m3u_path, "r");
  if (f == NULL)
    return 0;
  char *line = malloc(LINE_BUF_LEN);
  char *full = malloc(PATH_BUF_LEN);
  size_t cap = TRACKS_INITIAL;
  size_t n = 0;
  uint32_t *arr = malloc(cap * sizeof(uint32_t));
  if (line == NULL || full == NULL || arr == NULL) {
    ESP_LOGE(TAG, "m3u alloc failed");
    goto fail;
  }
  char dir[PATH_BUF_LEN];
  ipod_text_copy(dir, sizeof(dir), m3u_path);
  char *slash = strrchr(dir, '/');
  if (slash != NULL)
    *slash = '\0';

  bool first = true;
  while (fgets(line, LINE_BUF_LEN, f) != NULL) {
    char *s = line;
    if (first && strncmp(s, UTF8_BOM, strlen(UTF8_BOM)) == 0)
      s += strlen(UTF8_BOM);
    first = false;
    while (*s == ' ' || *s == '\t')
      s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' '))
      s[--len] = '\0';
    if (len == 0 || s[0] == '#')
      continue;
    for (char *c = s; *c != '\0'; c++) {
      if (*c == '\\')
        *c = '/';
    }
    if (s[0] == '/') {
      ipod_text_copy(full, PATH_BUF_LEN, s);
    } else {
      size_t dlen = strlen(dir);
      if (dlen + 1 + len >= PATH_BUF_LEN)
        continue; // too long for any FAT path we could open
      memcpy(full, dir, dlen);
      full[dlen] = '/';
      memcpy(full + dlen + 1, s, len + 1);
    }
    normalize_path(full);
    uint32_t idx = 0;
    if (!ipod_lib_find_path(lib, full, &idx))
      continue;
    if (n == cap) {
      uint32_t *grown = realloc(arr, cap * 2 * sizeof(uint32_t));
      if (grown == NULL) {
        ESP_LOGE(TAG, "m3u grow failed");
        break;
      }
      arr = grown;
      cap *= 2;
    }
    arr[n++] = idx;
  }
  free(line);
  free(full);
  fclose(f);
  if (n == 0) {
    free(arr);
    return 0;
  }
  *out = arr;
  return n;

fail:
  free(line);
  free(full);
  free(arr);
  fclose(f);
  return 0;
}

bool ipod_lib_write_m3u(const ipod_library_t *lib,
                        const char *m3u_path,
                        const uint32_t *tracks,
                        size_t count) {
  FILE *f = fopen(m3u_path, "w");
  if (f == NULL)
    return false;
  bool ok = fputs("#EXTM3U\n", f) >= 0;
  for (size_t i = 0; ok && i < count; i++) {
    const ipod_track_t *t = ipod_lib_track(lib, tracks[i]);
    if (t != NULL)
      ok = fprintf(f, "%s\n", ipod_lib_str(lib, t->path)) > 0;
  }
  return fclose(f) == 0 && ok;
}

void ipod_lib_totals(const ipod_library_t *lib, uint64_t *out_ms, uint64_t *out_bytes) {
  uint64_t ms = 0;
  uint64_t bytes = 0;
  for (uint32_t i = 0; lib != NULL && i < lib->ntracks; i++) {
    ms += lib->tracks[i].duration_ms;
    bytes += lib->tracks[i].file_size;
  }
  if (out_ms != NULL)
    *out_ms = ms;
  if (out_bytes != NULL)
    *out_bytes = bytes;
}

uint32_t ipod_lib_max_play_seq(const ipod_library_t *lib) {
  uint32_t m = 0;
  for (uint32_t i = 0; lib != NULL && i < lib->ntracks; i++) {
    if (lib->tracks[i].last_played > m)
      m = lib->tracks[i].last_played;
  }
  return m;
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

static uint32_t hash_bytes(const char *s, bool fold_case) {
  uint32_t h = FNV_OFFSET;
  for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
    h ^= fold_case ? (uint32_t)tolower(*p) : *p;
    h *= FNV_PRIME;
  }
  return h;
}

static uint32_t pool_add(ipod_library_t *lib, const char *s) {
  size_t len = strlen(s) + 1;
  if (lib->pool_len + len > lib->pool_cap) {
    size_t cap = lib->pool_cap;
    while (lib->pool_len + len > cap)
      cap *= 2;
    char *grown = realloc(lib->pool, cap);
    if (grown == NULL) {
      ESP_LOGE(TAG, "string pool grow failed");
      lib->is_oom = true;
      return UINT32_MAX;
    }
    lib->pool = grown;
    lib->pool_cap = cap;
  }
  uint32_t off = (uint32_t)lib->pool_len;
  memcpy(lib->pool + off, s, len);
  lib->pool_len += len;
  return off;
}

static bool rehash_strings(ipod_library_t *lib) {
  uint32_t nb = lib->nbuckets * 2;
  uint32_t *b = calloc(nb, sizeof(uint32_t));
  if (b == NULL) {
    ESP_LOGE(TAG, "rehash alloc failed");
    lib->is_oom = true;
    return false;
  }
  for (uint32_t i = 1; i < lib->nstr; i++) {
    uint32_t slot = lib->strs[i].hash & (nb - 1);
    lib->strs[i].next = b[slot];
    b[slot] = i + 1;
  }
  free(lib->buckets);
  lib->buckets = b;
  lib->nbuckets = nb;
  return true;
}

static ipod_str_t intern(ipod_library_t *lib, const char *s, bool is_text) {
  if (s == NULL || s[0] == '\0' || lib->is_oom)
    return IPOD_STR_EMPTY;
  uint32_t h = hash_bytes(s, false);
  for (uint32_t e = lib->buckets[h & (lib->nbuckets - 1)]; e != 0; e = lib->strs[e - 1].next) {
    const str_entry_t *se = &lib->strs[e - 1];
    if (se->hash == h && strcmp(lib->pool + se->raw, s) == 0)
      return e - 1;
  }
  if (lib->nstr == lib->str_cap) {
    str_entry_t *grown = realloc(lib->strs, lib->str_cap * 2 * sizeof(str_entry_t));
    if (grown == NULL) {
      ESP_LOGE(TAG, "string table grow failed");
      lib->is_oom = true;
      return IPOD_STR_EMPTY;
    }
    lib->strs = grown;
    lib->str_cap *= 2;
  }
  str_entry_t se = {.hash = h};
  se.raw = pool_add(lib, s);
  if (is_text) {
    char buf[TEXT_BUF_LEN];
    ipod_text_fold(s, buf, sizeof(buf));
    se.disp = strcmp(buf, s) == 0 ? se.raw : pool_add(lib, buf);
    ipod_text_sort_key(s, buf, sizeof(buf));
    se.key = pool_add(lib, buf);
  } else {
    se.disp = se.raw;
    se.key = se.raw;
  }
  if (se.raw == UINT32_MAX || se.disp == UINT32_MAX || se.key == UINT32_MAX)
    return IPOD_STR_EMPTY;
  uint32_t id = lib->nstr++;
  uint32_t slot = h & (lib->nbuckets - 1);
  se.next = lib->buckets[slot];
  lib->strs[id] = se;
  lib->buckets[slot] = id + 1;
  if (lib->nstr > lib->nbuckets)
    rehash_strings(lib);
  return id;
}

static bool add_track(ipod_library_t *lib, const track_rec_t *rec) {
  if (lib->ntracks == lib->track_cap) {
    uint32_t cap = lib->track_cap == 0 ? TRACKS_INITIAL : lib->track_cap * 2;
    ipod_track_t *grown = realloc(lib->tracks, cap * sizeof(ipod_track_t));
    if (grown == NULL) {
      ESP_LOGE(TAG, "track table grow failed");
      lib->is_oom = true;
      return false;
    }
    lib->tracks = grown;
    lib->track_cap = cap;
  }
  ipod_track_t t = rec->num;
  t.path = intern(lib, rec->path, false);

  if (rec->title != NULL && rec->title[0] != '\0') {
    t.title = intern(lib, rec->title, true);
  } else {
    // Untagged file: show its name without folder or extension, like iTunes.
    char name[TEXT_BUF_LEN];
    const char *slash = strrchr(rec->path, '/');
    ipod_text_copy(name, sizeof(name), slash != NULL ? slash + 1 : rec->path);
    char *dot = strrchr(name, '.');
    if (dot != NULL && dot != name)
      *dot = '\0';
    t.title = intern(lib, name, true);
  }
  const bool has_artist = rec->artist != NULL && rec->artist[0] != '\0';
  t.artist = intern(lib, has_artist ? rec->artist : IPOD_LIB_UNKNOWN_ARTIST, true);
  const bool has_album = rec->album != NULL && rec->album[0] != '\0';
  t.album = intern(lib, has_album ? rec->album : IPOD_LIB_UNKNOWN_ALBUM, true);
  if (rec->album_artist != NULL && rec->album_artist[0] != '\0')
    t.album_artist = intern(lib, rec->album_artist, true);
  else if (t.flags & IPOD_TRACK_COMPILATION)
    t.album_artist = intern(lib, IPOD_LIB_VARIOUS, true);
  else
    t.album_artist = t.artist;
  t.genre = intern(lib, rec->genre, true);
  t.composer = intern(lib, rec->composer, true);
  if (lib->is_oom || t.path == IPOD_STR_EMPTY)
    return false;
  lib->tracks[lib->ntracks++] = t;
  return true;
}

static bool add_playlist(ipod_library_t *lib, const char *path) {
  if (lib->nplaylists == lib->pl_cap) {
    uint32_t cap = lib->pl_cap == 0 ? PLAYLISTS_INITIAL : lib->pl_cap * 2;
    ipod_str_t *p = realloc(lib->pl_paths, cap * sizeof(ipod_str_t));
    if (p == NULL) {
      lib->is_oom = true;
      return false;
    }
    lib->pl_paths = p;
    ipod_str_t *nm = realloc(lib->pl_names, cap * sizeof(ipod_str_t));
    if (nm == NULL) {
      lib->is_oom = true;
      return false;
    }
    lib->pl_names = nm;
    lib->pl_cap = cap;
  }
  char name[TEXT_BUF_LEN];
  const char *slash = strrchr(path, '/');
  ipod_text_copy(name, sizeof(name), slash != NULL ? slash + 1 : path);
  char *dot = strrchr(name, '.');
  if (dot != NULL && dot != name)
    *dot = '\0';
  ipod_str_t p = intern(lib, path, false);
  ipod_str_t nm = intern(lib, name, true);
  if (lib->is_oom)
    return false;
  lib->pl_paths[lib->nplaylists] = p;
  lib->pl_names[lib->nplaylists] = nm;
  lib->nplaylists++;
  return true;
}

static void sort_playlists(ipod_library_t *lib) {
  // Insertion sort: playlist counts are tiny.
  for (uint32_t i = 1; i < lib->nplaylists; i++) {
    ipod_str_t p = lib->pl_paths[i];
    ipod_str_t nm = lib->pl_names[i];
    uint32_t j = i;
    while (j > 0 &&
           ipod_text_key_cmp(ipod_lib_key(lib, lib->pl_names[j - 1]), ipod_lib_key(lib, nm)) > 0) {
      lib->pl_paths[j] = lib->pl_paths[j - 1];
      lib->pl_names[j] = lib->pl_names[j - 1];
      j--;
    }
    lib->pl_paths[j] = p;
    lib->pl_names[j] = nm;
  }
}

static bool build_path_index(ipod_library_t *lib) {
  uint32_t cap = 16;
  while (cap < lib->ntracks * 2)
    cap *= 2;
  uint32_t *slots = calloc(cap, sizeof(uint32_t));
  if (slots == NULL) {
    ESP_LOGE(TAG, "path index alloc failed");
    return false;
  }
  for (uint32_t i = 0; i < lib->ntracks; i++) {
    uint32_t h = hash_bytes(ipod_lib_str(lib, lib->tracks[i].path), true) & (cap - 1);
    while (slots[h] != 0)
      h = (h + 1) & (cap - 1);
    slots[h] = i + 1;
  }
  free(lib->path_slots);
  lib->path_slots = slots;
  lib->path_cap = cap;
  return true;
}

static bool track_matches(const ipod_track_t *t, const ipod_filter_t *f) {
  if (f->album_artist != IPOD_STR_EMPTY && t->album_artist != f->album_artist)
    return false;
  if (f->album != IPOD_STR_EMPTY && t->album != f->album)
    return false;
  if (f->genre != IPOD_STR_EMPTY && t->genre != f->genre)
    return false;
  if (f->composer != IPOD_STR_EMPTY && t->composer != f->composer)
    return false;
  if (f->compilations_only && !(t->flags & IPOD_TRACK_COMPILATION))
    return false;
  return true;
}

static int cmp_str_ids(const ipod_library_t *lib, ipod_str_t a, ipod_str_t b) {
  if (a == b)
    return 0;
  int c = ipod_text_key_cmp(ipod_lib_key(lib, a), ipod_lib_key(lib, b));
  if (c != 0)
    return c;
  return a < b ? -1 : 1; // equal keys, distinct strings: keep each group contiguous
}

static int cmp_index(uint32_t a, uint32_t b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

static int cmp_title(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  int c = ipod_text_key_cmp(ipod_lib_key(lib, lib->tracks[a].title),
                            ipod_lib_key(lib, lib->tracks[b].title));
  if (c != 0)
    return c;
  c = cmp_str_ids(lib, lib->tracks[a].album_artist, lib->tracks[b].album_artist);
  return c != 0 ? c : cmp_index(a, b);
}

static int cmp_album_order(uint32_t a, uint32_t b, const void *ctx) {
  const ipod_library_t *lib = ctx;
  const ipod_track_t *ta = &lib->tracks[a];
  const ipod_track_t *tb = &lib->tracks[b];
  int c = cmp_str_ids(lib, ta->album, tb->album);
  if (c != 0)
    return c;
  c = cmp_str_ids(lib, ta->album_artist, tb->album_artist);
  if (c != 0)
    return c;
  if (ta->disc_no != tb->disc_no)
    return ta->disc_no < tb->disc_no ? -1 : 1;
  if (ta->track_no != tb->track_no)
    return ta->track_no < tb->track_no ? -1 : 1;
  c = ipod_text_key_cmp(ipod_lib_key(lib, ta->title), ipod_lib_key(lib, tb->title));
  return c != 0 ? c : cmp_index(a, b);
}

static int cmp_group(uint32_t a, uint32_t b, const void *ctx) {
  const group_ctx_t *g = ctx;
  int c = cmp_str_ids(g->lib,
                      ipod_lib_field_value(g->lib, g->field, a),
                      ipod_lib_field_value(g->lib, g->field, b));
  if (c == 0 && g->field == IPOD_FIELD_ALBUM)
    c = cmp_str_ids(g->lib, g->lib->tracks[a].album_artist, g->lib->tracks[b].album_artist);
  return c != 0 ? c : cmp_index(a, b);
}

static bool same_group(const ipod_library_t *lib, ipod_field_t field, uint32_t a, uint32_t b) {
  if (ipod_lib_field_value(lib, field, a) != ipod_lib_field_value(lib, field, b))
    return false;
  if (field == IPOD_FIELD_ALBUM)
    return lib->tracks[a].album_artist == lib->tracks[b].album_artist;
  return true;
}

static void insertion_sort(uint32_t *arr, size_t n, idx_cmp_fn cmp, const void *ctx) {
  for (size_t i = 1; i < n; i++) {
    uint32_t v = arr[i];
    size_t j = i;
    while (j > 0 && cmp(arr[j - 1], v, ctx) > 0) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = v;
  }
}

// Bottom-up merge sort with a context pointer (qsort_r is not portable to newlib).
static void sort_idx(uint32_t *arr, size_t n, idx_cmp_fn cmp, const void *ctx) {
  if (n < 2)
    return;
  uint32_t *tmp = malloc(n * sizeof(uint32_t));
  if (tmp == NULL) {
    insertion_sort(arr, n, cmp, ctx);
    return;
  }
  uint32_t *src = arr;
  uint32_t *dst = tmp;
  for (size_t width = 1; width < n; width *= 2) {
    for (size_t lo = 0; lo < n; lo += 2 * width) {
      size_t mid = lo + width < n ? lo + width : n;
      size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
      size_t i = lo;
      size_t j = mid;
      size_t k = lo;
      while (i < mid && j < hi)
        dst[k++] = cmp(src[i], src[j], ctx) <= 0 ? src[i++] : src[j++];
      while (i < mid)
        dst[k++] = src[i++];
      while (j < hi)
        dst[k++] = src[j++];
    }
    uint32_t *swap = src;
    src = dst;
    dst = swap;
  }
  if (src != arr)
    memcpy(arr, src, n * sizeof(uint32_t));
  free(tmp);
}

static bool has_ext(const char *name, const char *ext) {
  size_t n = strlen(name);
  size_t e = strlen(ext);
  return n > e && ipod_text_equal_ci(name + n - e, ext);
}

// Collapse "//", "/./" and "/x/../" segments in place.
static void normalize_path(char *path) {
  char *segs[64];
  int nseg = 0;
  char *save = NULL;
  char buf[PATH_BUF_LEN];
  ipod_text_copy(buf, sizeof(buf), path);
  for (char *tok = strtok_r(buf, "/", &save); tok != NULL && nseg < 64;
       tok = strtok_r(NULL, "/", &save)) {
    if (strcmp(tok, ".") == 0)
      continue;
    if (strcmp(tok, "..") == 0) {
      if (nseg > 0)
        nseg--;
      continue;
    }
    segs[nseg++] = tok;
  }
  const bool is_absolute = path[0] == '/';
  size_t w = 0;
  for (int i = 0; i < nseg && w + 1 < PATH_BUF_LEN; i++) {
    if (i > 0 || is_absolute)
      path[w++] = '/';
    size_t len = strlen(segs[i]);
    if (w + len >= PATH_BUF_LEN)
      len = PATH_BUF_LEN - 1 - w;
    memcpy(path + w, segs[i], len);
    w += len;
  }
  if (w == 0)
    path[w++] = is_absolute ? '/' : '.';
  path[w] = '\0';
}
