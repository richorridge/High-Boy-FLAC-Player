// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipod_dsp.h"

#define LINE_LEN 96

void ipod_settings_defaults(ipod_settings_t *s) {
  memset(s, 0, sizeof(*s));
  s->volume = IPOD_VOLUME_DEFAULT;
  s->volume_limit = IPOD_VOLUME_MAX;
  s->shuffle = IPOD_SHUFFLE_OFF;
  s->repeat = IPOD_REPEAT_OFF;
  s->eq_preset = IPOD_EQ_OFF;
  s->sound_check = false;
  s->clicker = true;
}

static int clamp_int(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

bool ipod_settings_load(ipod_settings_t *s, const char *path) {
  FILE *f = fopen(path, "r");
  if (f == NULL)
    return false;
  char line[LINE_LEN];
  while (fgets(line, sizeof(line), f) != NULL) {
    char *eq = strchr(line, '=');
    if (eq == NULL)
      continue;
    *eq = '\0';
    int v = atoi(eq + 1);
    if (strcmp(line, "volume") == 0)
      s->volume = (uint8_t)clamp_int(v, 0, IPOD_VOLUME_MAX);
    else if (strcmp(line, "volume_limit") == 0)
      s->volume_limit = (uint8_t)clamp_int(v, 0, IPOD_VOLUME_MAX);
    else if (strcmp(line, "shuffle") == 0)
      s->shuffle = (ipod_shuffle_t)clamp_int(v, 0, IPOD_SHUFFLE_COUNT - 1);
    else if (strcmp(line, "repeat") == 0)
      s->repeat = (ipod_repeat_t)clamp_int(v, 0, IPOD_REPEAT_COUNT - 1);
    else if (strcmp(line, "eq") == 0)
      s->eq_preset = clamp_int(v, 0, ipod_eq_count() - 1);
    else if (strcmp(line, "sound_check") == 0)
      s->sound_check = v != 0;
    else if (strcmp(line, "clicker") == 0)
      s->clicker = v != 0;
  }
  fclose(f);
  if (s->volume > s->volume_limit)
    s->volume = s->volume_limit;
  return true;
}

bool ipod_settings_save(const ipod_settings_t *s, const char *path) {
  FILE *f = fopen(path, "w");
  if (f == NULL)
    return false;
  int n = fprintf(f,
                  "volume=%u\nvolume_limit=%u\nshuffle=%d\nrepeat=%d\neq=%d\nsound_check=%d\n"
                  "clicker=%d\n",
                  (unsigned)s->volume,
                  (unsigned)s->volume_limit,
                  (int)s->shuffle,
                  (int)s->repeat,
                  s->eq_preset,
                  (int)s->sound_check,
                  (int)s->clicker);
  return fclose(f) == 0 && n > 0;
}
