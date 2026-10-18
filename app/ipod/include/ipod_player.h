// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_PLAYER_H
#define IPOD_PLAYER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "ipod_library.h"
#include "ipod_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief "Previous" restarts the song instead when played longer than this. */
#define IPOD_PLAYER_RESTART_MS 3000u

typedef enum {
  IPOD_PLAYER_STOPPED = 0,
  IPOD_PLAYER_PLAYING,
  IPOD_PLAYER_PAUSED,
} ipod_play_state_t;

typedef struct {
  ipod_play_state_t state;
  bool has_track;
  uint32_t track; ///< library index of the current song
  uint32_t pos_ms;
  uint32_t dur_ms;
  size_t queue_pos; ///< 0-based position in the Now Playing list
  size_t queue_count;
  uint32_t track_seq; ///< bumps whenever the current song changes
  uint32_t end_seq;   ///< bumps when the list plays out (iPod leaves Now Playing)
  uint32_t error_seq; ///< bumps when a file could not be played
  uint32_t out_rate;  ///< rate the speaker is running at (0 when idle)
  ipod_shuffle_t shuffle;
  ipod_repeat_t repeat;
} ipod_player_status_t;

/** @brief Start the decoder task. @p lib must outlive the player (see set_library). */
esp_err_t ipod_player_start(ipod_library_t *lib);

/** @brief Stop playback and end the decoder task (blocks up to ~1 s). */
void ipod_player_shutdown(void);

/** @brief Swap the library (after a rescan); stops playback and clears the list. */
void ipod_player_set_library(ipod_library_t *lib);

/**
 * @brief Replace the Now Playing list and start playing.
 * @param groups  optional album id per track, for album shuffle
 * @param start   index into @p tracks; SIZE_MAX = random start (Shuffle Songs)
 */
void ipod_player_play_list(const uint32_t *tracks,
                           const uint32_t *groups,
                           size_t count,
                           size_t start);

void ipod_player_toggle_pause(void);
void ipod_player_set_paused(bool paused);
void ipod_player_stop(void);
void ipod_player_next(void);
/** @brief iPod "previous": restart the song if past 3 s, otherwise go back one. */
void ipod_player_prev(void);
void ipod_player_seek_to(uint32_t ms);
/** @brief Relative seek (fast-forward / rewind). Crossing the end skips on. */
void ipod_player_seek_by(int32_t delta_ms);

void ipod_player_set_shuffle(ipod_shuffle_t mode);
void ipod_player_set_repeat(ipod_repeat_t mode);
void ipod_player_set_eq(int preset);
void ipod_player_set_sound_check(bool enabled);
void ipod_player_set_volume(uint8_t pct);

/** @brief Clicker tick: mixed into the stream while playing, else a UI click. */
void ipod_player_click(void);

void ipod_player_get_status(ipod_player_status_t *out);

/** @brief Copy of the Now Playing list in play order (caller frees). */
size_t ipod_player_copy_queue(uint32_t **out);

/** @brief True (once) when play counts / ratings changed since the last call. */
bool ipod_player_take_stats_dirty(void);

#ifdef __cplusplus
}
#endif

#endif // IPOD_PLAYER_H
