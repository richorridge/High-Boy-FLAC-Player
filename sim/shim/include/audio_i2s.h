// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: the subset of the TentacleOS header of the same
// name used by the app, with identical declarations.

#ifndef AUDIO_I2S_H
#define AUDIO_I2S_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_i2s_init(void);
void audio_i2s_set_volume(uint8_t pct);
esp_err_t audio_i2s_stream_start(uint32_t sample_rate);
int audio_i2s_stream_write(const int16_t *pcm, int n_samples);
void audio_i2s_stream_stop(void);
void audio_click(void);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_I2S_H
