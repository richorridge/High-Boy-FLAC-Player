// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_queue.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define RNG_FALLBACK_SEED 0x9E3779B9u

static const char *TAG = "IPOD_QUEUE";

static uint32_t rng_next(ipod_queue_t *q);
static void build_order(ipod_queue_t *q, size_t first);

void ipod_queue_init(ipod_queue_t *q, uint32_t seed) {
  memset(q, 0, sizeof(*q));
  q->rng = seed != 0 ? seed : RNG_FALLBACK_SEED;
}

void ipod_queue_free(ipod_queue_t *q) {
  free(q->tracks);
  free(q->groups);
  free(q->order);
  q->tracks = NULL;
  q->groups = NULL;
  q->order = NULL;
  q->count = 0;
  q->pos = 0;
}

bool ipod_queue_set(
    ipod_queue_t *q, const uint32_t *tracks, const uint32_t *groups, size_t count, size_t start) {
  ipod_queue_free(q);
  if (count == 0 || tracks == NULL)
    return false;
  q->tracks = malloc(count * sizeof(uint32_t));
  q->order = malloc(count * sizeof(uint32_t));
  if (groups != NULL)
    q->groups = malloc(count * sizeof(uint32_t));
  if (q->tracks == NULL || q->order == NULL || (groups != NULL && q->groups == NULL)) {
    ESP_LOGE(TAG, "queue alloc failed (%u tracks)", (unsigned)count);
    ipod_queue_free(q);
    return false;
  }
  memcpy(q->tracks, tracks, count * sizeof(uint32_t));
  if (groups != NULL)
    memcpy(q->groups, groups, count * sizeof(uint32_t));
  q->count = count;
  if (start != SIZE_MAX && start >= count)
    start = 0;
  build_order(q, start);
  return true;
}

void ipod_queue_set_shuffle(ipod_queue_t *q, ipod_shuffle_t mode) {
  if (mode >= IPOD_SHUFFLE_COUNT)
    mode = IPOD_SHUFFLE_OFF;
  q->shuffle = mode;
  if (q->count == 0)
    return;
  build_order(q, q->order[q->pos]);
}

bool ipod_queue_current(const ipod_queue_t *q, uint32_t *out_track) {
  if (q->count == 0)
    return false;
  if (out_track != NULL)
    *out_track = q->tracks[q->order[q->pos]];
  return true;
}

bool ipod_queue_next(ipod_queue_t *q, bool by_user) {
  if (q->count == 0)
    return false;
  if (!by_user && q->repeat == IPOD_REPEAT_ONE)
    return true;
  if (q->pos + 1 < q->count) {
    q->pos++;
    return true;
  }
  if (q->repeat == IPOD_REPEAT_OFF)
    return false;
  if (q->shuffle != IPOD_SHUFFLE_OFF)
    build_order(q, SIZE_MAX); // a fresh shuffle for the next lap
  q->pos = 0;
  return true;
}

bool ipod_queue_prev(ipod_queue_t *q) {
  if (q->count == 0)
    return false;
  if (q->pos > 0)
    q->pos--;
  else if (q->repeat == IPOD_REPEAT_ALL)
    q->pos = q->count - 1;
  return true;
}

static uint32_t rng_next(ipod_queue_t *q) {
  uint32_t x = q->rng; // xorshift32
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  q->rng = x;
  return x;
}

static void shuffle_range(ipod_queue_t *q, uint32_t *a, size_t n) {
  for (size_t i = n; i > 1; i--) {
    size_t j = rng_next(q) % i;
    uint32_t t = a[i - 1];
    a[i - 1] = a[j];
    a[j] = t;
  }
}

// Album shuffle: albums play in random order, songs within an album in order.
static bool build_album_order(ipod_queue_t *q, size_t first) {
  uint32_t *uniq = malloc(q->count * sizeof(uint32_t));
  if (uniq == NULL) {
    ESP_LOGE(TAG, "album shuffle alloc failed");
    return false;
  }
  size_t ng = 0;
  for (size_t i = 0; i < q->count; i++) {
    bool seen = false;
    for (size_t g = 0; g < ng && !seen; g++)
      seen = uniq[g] == q->groups[i];
    if (!seen)
      uniq[ng++] = q->groups[i];
  }
  shuffle_range(q, uniq, ng);
  if (first != SIZE_MAX) {
    for (size_t g = 0; g < ng; g++) {
      if (uniq[g] == q->groups[first]) {
        uint32_t t = uniq[0];
        uniq[0] = uniq[g];
        uniq[g] = t;
        break;
      }
    }
  }
  size_t w = 0;
  q->pos = 0;
  for (size_t g = 0; g < ng; g++) {
    for (size_t i = 0; i < q->count; i++) {
      if (q->groups[i] != uniq[g])
        continue;
      if (i == first)
        q->pos = w;
      q->order[w++] = (uint32_t)i;
    }
  }
  free(uniq);
  return true;
}

static void build_order(ipod_queue_t *q, size_t first) {
  for (size_t i = 0; i < q->count; i++)
    q->order[i] = (uint32_t)i;
  q->pos = 0;
  switch (q->shuffle) {
    case IPOD_SHUFFLE_SONGS:
      if (first != SIZE_MAX) {
        q->order[0] = (uint32_t)first;
        q->order[first] = 0;
        shuffle_range(q, q->order + 1, q->count - 1);
      } else {
        shuffle_range(q, q->order, q->count);
      }
      break;
    case IPOD_SHUFFLE_ALBUMS:
      if (q->groups != NULL && build_album_order(q, first))
        break;
      // No album info: fall back to song shuffle.
      if (first != SIZE_MAX) {
        q->order[0] = (uint32_t)first;
        q->order[first] = 0;
        shuffle_range(q, q->order + 1, q->count - 1);
      } else {
        shuffle_range(q, q->order, q->count);
      }
      break;
    default:
      q->pos = first != SIZE_MAX ? first : 0;
      break;
  }
}
