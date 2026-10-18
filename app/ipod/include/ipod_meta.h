// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_META_H
#define IPOD_META_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_META_TEXT_MAX 128
#define IPOD_META_MIME_MAX 32

/** @brief Picture block types we care about (FLAC/ID3 APIC numbering). */
#define IPOD_META_PIC_FRONT_COVER 3

typedef struct {
  // STREAMINFO
  uint32_t sample_rate;
  uint8_t channels;
  uint8_t bits_per_sample;
  uint64_t total_frames; ///< 0 when the encoder did not know the length
  // VORBIS_COMMENT (UTF-8, as stored)
  char title[IPOD_META_TEXT_MAX];
  char artist[IPOD_META_TEXT_MAX];
  char album[IPOD_META_TEXT_MAX];
  char album_artist[IPOD_META_TEXT_MAX];
  char genre[IPOD_META_TEXT_MAX];
  char composer[IPOD_META_TEXT_MAX];
  uint16_t track_no;
  uint16_t disc_no;
  uint16_t year;
  bool is_compilation;
  bool has_rg_track;
  bool has_rg_album;
  float rg_track_gain_db;
  float rg_album_gain_db;
  // PICTURE (embedded cover art location inside the file)
  uint32_t picture_offset; ///< absolute file offset of the image bytes, 0 = none
  uint32_t picture_size;
  char picture_mime[IPOD_META_MIME_MAX];
} ipod_meta_t;

typedef enum {
  IPOD_META_OK = 0,
  IPOD_META_NOT_FLAC, ///< missing, unreadable or not a FLAC stream
  IPOD_META_BUSY,     ///< every SD file slot was taken: try again later
} ipod_meta_status_t;

/**
 * @brief Parse the metadata blocks of a FLAC file.
 *
 * Only the header is read; audio frames are never touched, so this is cheap
 * enough to run over a whole SD card. A leading ID3v2 tag is skipped.
 */
ipod_meta_status_t ipod_meta_read(const char *path, ipod_meta_t *out_meta);

/** @brief ipod_meta_read() == IPOD_META_OK. */
bool ipod_meta_read_flac(const char *path, ipod_meta_t *out_meta);

/** @brief Duration in milliseconds derived from STREAMINFO (0 if unknown). */
uint32_t ipod_meta_duration_ms(const ipod_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif // IPOD_META_H
