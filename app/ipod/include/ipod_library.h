// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_LIBRARY_H
#define IPOD_LIBRARY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Interned string id. 0 is always the empty string. */
typedef uint32_t ipod_str_t;
#define IPOD_STR_EMPTY 0u

#define IPOD_RG_NONE           INT16_MIN
#define IPOD_RATING_MAX        5
#define IPOD_TRACK_COMPILATION 0x01u

#define IPOD_LIB_UNKNOWN_ARTIST "Unknown Artist"
#define IPOD_LIB_UNKNOWN_ALBUM  "Unknown Album"
#define IPOD_LIB_VARIOUS        "Various Artists"

typedef struct {
  ipod_str_t path;
  ipod_str_t title;
  ipod_str_t artist;       ///< track artist, as tagged (Unknown Artist if missing)
  ipod_str_t album;        ///< Unknown Album if missing
  ipod_str_t album_artist; ///< effective album artist used for grouping
  ipod_str_t genre;
  ipod_str_t composer;
  uint32_t duration_ms;
  uint32_t file_size;
  uint32_t mtime;
  uint32_t picture_offset; ///< embedded cover art (0 = none)
  uint32_t picture_size;
  uint32_t sample_rate;
  uint16_t track_no;
  uint16_t disc_no;
  uint16_t year;
  int16_t rg_track_cdb; ///< ReplayGain track gain in 1/100 dB, IPOD_RG_NONE if absent
  int16_t rg_album_cdb;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint8_t flags;
  // Listening statistics (persisted with the cache).
  uint8_t rating; ///< 0..IPOD_RATING_MAX stars
  uint16_t play_count;
  uint32_t last_played; ///< monotonically increasing play sequence, 0 = never
} ipod_track_t;

typedef enum {
  IPOD_FIELD_ARTIST = 0, ///< effective album artist
  IPOD_FIELD_ALBUM,      ///< (album, album artist) pairs
  IPOD_FIELD_GENRE,
  IPOD_FIELD_COMPOSER,
  IPOD_FIELD_COUNT,
} ipod_field_t;

typedef enum {
  IPOD_ORDER_TITLE = 0, ///< song title
  IPOD_ORDER_ALBUM,     ///< album, disc, track number, title
  IPOD_ORDER_COUNT,
} ipod_order_t;

/** @brief Constraint set for queries; IPOD_STR_EMPTY in a field means "any". */
typedef struct {
  ipod_str_t album_artist;
  ipod_str_t album;
  ipod_str_t genre;
  ipod_str_t composer;
  bool compilations_only;
} ipod_filter_t;

typedef enum {
  IPOD_SMART_RECENTLY_ADDED = 0,
  IPOD_SMART_TOP_PLAYED,
  IPOD_SMART_RECENTLY_PLAYED,
  IPOD_SMART_TOP_RATED,
  IPOD_SMART_COUNT,
} ipod_smart_t;

typedef enum {
  IPOD_HIT_ARTIST = 0,
  IPOD_HIT_ALBUM,
  IPOD_HIT_SONG,
} ipod_hit_kind_t;

typedef struct {
  ipod_hit_kind_t kind;
  uint32_t track; ///< the song, or a representative track of the artist/album
} ipod_hit_t;

/** @brief Shared between the scanning task and the UI, hence atomic. */
typedef struct {
  bool skip_stats;         ///< input: do not read statistics from `previous`
  atomic_bool cancel;      ///< set by the caller to abort a scan
  atomic_uint files_found; ///< audio files discovered so far
  atomic_uint files_parsed;
} ipod_scan_progress_t;

typedef struct ipod_library ipod_library_t;

ipod_library_t *ipod_lib_create(void);
void ipod_lib_destroy(ipod_library_t *lib);

/**
 * @brief Walk @p root recursively for .flac and .m3u/.m3u8 files.
 *
 * Tracks whose size and mtime match an entry of @p previous are copied from it
 * (with their statistics) instead of being re-parsed, so a rescan of an
 * unchanged card only costs a directory walk.
 *
 * @return New library, or NULL on allocation failure or cancellation.
 */
ipod_library_t *
ipod_lib_scan(const char *root, const ipod_library_t *previous, ipod_scan_progress_t *progress);

/** @brief True when both libraries hold the same files (paths, sizes, mtimes). */
bool ipod_lib_same_files(const ipod_library_t *a, const ipod_library_t *b);

/** @brief Copy ratings and play counts from @p src to matching paths in @p dst. */
void ipod_lib_copy_stats(ipod_library_t *dst, const ipod_library_t *src);

/** @brief Persist the library (and statistics) to a binary cache file. */
bool ipod_lib_save(const ipod_library_t *lib, const char *path);

/** @brief Load a cache written by ipod_lib_save(); NULL if missing/corrupt. */
ipod_library_t *ipod_lib_load(const char *path);

size_t ipod_lib_count(const ipod_library_t *lib);
const ipod_track_t *ipod_lib_track(const ipod_library_t *lib, uint32_t index);
ipod_track_t *ipod_lib_track_mut(ipod_library_t *lib, uint32_t index);

/** @brief Raw UTF-8 text of an interned string (file paths, tags). */
const char *ipod_lib_str(const ipod_library_t *lib, ipod_str_t id);
/** @brief ASCII-folded text of an interned string, ready to draw. */
const char *ipod_lib_disp(const ipod_library_t *lib, ipod_str_t id);
/** @brief Sort key of an interned string. */
const char *ipod_lib_key(const ipod_library_t *lib, ipod_str_t id);

/** @brief Find a track by path (case-insensitive). Returns false if absent. */
bool ipod_lib_find_path(const ipod_library_t *lib, const char *path, uint32_t *out_index);

/**
 * @brief Tracks matching @p filter in @p order.
 * @param out  malloc'd array of track indices (caller frees); NULL when empty.
 */
size_t ipod_lib_query_tracks(const ipod_library_t *lib,
                             const ipod_filter_t *filter,
                             ipod_order_t order,
                             uint32_t **out);

/**
 * @brief Distinct values of @p field among tracks matching @p filter, sorted.
 *        Each value is returned as a representative track index.
 */
size_t ipod_lib_query_groups(const ipod_library_t *lib,
                             ipod_field_t field,
                             const ipod_filter_t *filter,
                             uint32_t **out);

/** @brief Display label of a group value of @p field, taken from @p track. */
ipod_str_t ipod_lib_field_value(const ipod_library_t *lib, ipod_field_t field, uint32_t track);

/** @brief Count tracks with the compilation flag set. */
size_t ipod_lib_compilation_count(const ipod_library_t *lib);

/** @brief Smart playlists (Recently Added, Top 25 Most Played, ...). */
size_t ipod_lib_smart(const ipod_library_t *lib, ipod_smart_t kind, uint32_t **out);

/** @brief Case-insensitive search over song titles, artists and albums. */
size_t ipod_lib_search(const ipod_library_t *lib, const char *needle, size_t max, ipod_hit_t **out);

/** @brief Playlist files discovered on the card. */
size_t ipod_lib_playlist_count(const ipod_library_t *lib);
const char *ipod_lib_playlist_name(const ipod_library_t *lib, size_t index);
size_t ipod_lib_playlist_tracks(const ipod_library_t *lib, size_t index, uint32_t **out);

/** @brief Read an m3u file and resolve its entries to library tracks. */
size_t ipod_lib_read_m3u(const ipod_library_t *lib, const char *m3u_path, uint32_t **out);
/** @brief Write tracks as an m3u file of absolute paths. */
bool ipod_lib_write_m3u(const ipod_library_t *lib,
                        const char *m3u_path,
                        const uint32_t *tracks,
                        size_t count);

/** @brief Total duration and size of all tracks. */
void ipod_lib_totals(const ipod_library_t *lib, uint64_t *out_ms, uint64_t *out_bytes);

/** @brief Highest last_played sequence in the library (next play uses +1). */
uint32_t ipod_lib_max_play_seq(const ipod_library_t *lib);

#ifdef __cplusplus
}
#endif

#endif // IPOD_LIBRARY_H
