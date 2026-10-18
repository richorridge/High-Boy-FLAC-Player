// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_ART_H
#define IPOD_ART_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Decoded cover art, RGB565 (native endian, as LVGL expects). */
typedef struct {
  uint16_t w;
  uint16_t h;
  uint16_t *pixels;     ///< w*h; NULL when the song has no usable art
  uint16_t refl_h;      ///< rows of mirrored reflection (iPod Classic style)
  uint16_t *reflection; ///< w*refl_h, already faded into @p bg
} ipod_art_image_t;

esp_err_t ipod_art_start(void);
void ipod_art_shutdown(void);

/**
 * @brief Ask for the art of a song, scaled to fit a @p box x @p box square.
 *        Newer requests supersede older ones still waiting.
 * @param pic_offset/pic_size  embedded FLAC PICTURE (0 = look for cover.jpg etc.)
 * @param bg  RGB565 background the reflection fades into
 * @return request id to poll with ipod_art_take()
 */
uint32_t ipod_art_request(const char *flac_path,
                          uint32_t pic_offset,
                          uint32_t pic_size,
                          uint16_t box,
                          uint16_t refl_h,
                          uint16_t bg);

/**
 * @brief Collect a finished request. On true, ownership of the buffers moves
 *        to the caller (free with ipod_art_image_free).
 */
bool ipod_art_take(uint32_t request_id, ipod_art_image_t *out);

void ipod_art_image_free(ipod_art_image_t *img);

/**
 * @brief Synchronous decode used by the worker (exposed for tests).
 */
bool ipod_art_decode(const char *flac_path,
                     uint32_t pic_offset,
                     uint32_t pic_size,
                     uint16_t box,
                     uint16_t refl_h,
                     uint16_t bg,
                     ipod_art_image_t *out);

#ifdef __cplusplus
}
#endif

#endif // IPOD_ART_H
