// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_QUEUE_H
#define IPOD_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  IPOD_SHUFFLE_OFF = 0,
  IPOD_SHUFFLE_SONGS,
  IPOD_SHUFFLE_ALBUMS,
  IPOD_SHUFFLE_COUNT,
} ipod_shuffle_t;

typedef enum {
  IPOD_REPEAT_OFF = 0,
  IPOD_REPEAT_ONE,
  IPOD_REPEAT_ALL,
  IPOD_REPEAT_COUNT,
} ipod_repeat_t;

/**
 * @brief The "Now Playing" list: the tracks the user picked, the order they
 *        play in (which differs when shuffling) and the current position.
 */
typedef struct {
  uint32_t *tracks; ///< library track indices, in the order they were listed
  uint32_t *groups; ///< album id per track (for album shuffle), may be NULL
  uint32_t *order;  ///< play order: indices into tracks[]
  size_t count;
  size_t pos; ///< position in order[]
  ipod_shuffle_t shuffle;
  ipod_repeat_t repeat;
  uint32_t rng;
} ipod_queue_t;

void ipod_queue_init(ipod_queue_t *q, uint32_t seed);
void ipod_queue_free(ipod_queue_t *q);

/**
 * @brief Replace the queue. @p start is an index into @p tracks; with shuffle
 *        on it is played first and the rest is shuffled after it. When
 *        @p start is SIZE_MAX and shuffle is on, the first track is random too.
 * @param groups  optional album id per track, used by album shuffle.
 */
bool ipod_queue_set(
    ipod_queue_t *q, const uint32_t *tracks, const uint32_t *groups, size_t count, size_t start);

/** @brief Change shuffle mode, keeping the current track playing. */
void ipod_queue_set_shuffle(ipod_queue_t *q, ipod_shuffle_t mode);

/** @brief Track at the current position, or false when the queue is empty. */
bool ipod_queue_current(const ipod_queue_t *q, uint32_t *out_track);

/**
 * @brief Advance after a track ends (@p by_user false) or on a skip (true).
 *        Repeat One only holds the track for natural ends; Repeat All wraps.
 * @return false when the end of the list is reached (playback should stop).
 */
bool ipod_queue_next(ipod_queue_t *q, bool by_user);

/** @brief Step back one track. Wraps with Repeat All, otherwise stays at 0. */
bool ipod_queue_prev(ipod_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif // IPOD_QUEUE_H
