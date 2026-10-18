// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_meta.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ipod_text.h"

#define FLAC_MAGIC        "fLaC"
#define FLAC_MAGIC_LEN    4
#define ID3_MAGIC         "ID3"
#define ID3_HEADER_LEN    10
#define ID3_FOOTER_FLAG   0x10
#define ID3_FOOTER_LEN    10
#define BLOCK_HEADER_LEN  4
#define BLOCK_LAST_FLAG   0x80
#define BLOCK_TYPE_MASK   0x7F
#define BLOCK_STREAMINFO  0
#define BLOCK_VORBIS      4
#define BLOCK_PICTURE     6
#define STREAMINFO_LEN    34
#define MAX_BLOCKS        128
#define MAX_COMMENTS      1024
#define COMMENT_BUF_LEN   512
#define PICTURE_FIXED_LEN 16 // width, height, depth, colours
#define PICTURE_MAX_FIELD (64u * 1024u)
#define MS_PER_SEC        1000u
#define PIC_TYPE_UNSET    0xFFFFFFFFu

typedef struct {
  FILE *f;
  ipod_meta_t *meta;
  uint32_t pic_type;
} parse_ctx_t;

static bool read_be32(FILE *f, uint32_t *out);
static bool read_le32(FILE *f, uint32_t *out);
static bool skip_bytes(FILE *f, uint32_t n);
static bool parse_streaminfo(parse_ctx_t *ctx, uint32_t len);
static bool parse_vorbis(parse_ctx_t *ctx, uint32_t len);
static bool parse_picture(parse_ctx_t *ctx, uint32_t len);
static void apply_comment(ipod_meta_t *meta, char *kv);
static float parse_gain_db(const char *s);

bool ipod_meta_read_flac(const char *path, ipod_meta_t *out_meta) {
  if (path == NULL || out_meta == NULL)
    return false;
  memset(out_meta, 0, sizeof(*out_meta));
  FILE *f = fopen(path, "rb");
  if (f == NULL)
    return false;

  parse_ctx_t ctx = {.f = f, .meta = out_meta, .pic_type = PIC_TYPE_UNSET};
  bool ok = false;
  bool has_streaminfo = false;
  uint8_t hdr[ID3_HEADER_LEN];

  if (fread(hdr, 1, FLAC_MAGIC_LEN, f) != FLAC_MAGIC_LEN)
    goto cleanup;
  if (memcmp(hdr, ID3_MAGIC, strlen(ID3_MAGIC)) == 0) {
    if (fread(hdr + FLAC_MAGIC_LEN, 1, ID3_HEADER_LEN - FLAC_MAGIC_LEN, f) !=
        ID3_HEADER_LEN - FLAC_MAGIC_LEN)
      goto cleanup;
    uint32_t size = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                    ((uint32_t)(hdr[8] & 0x7F) << 7) | (uint32_t)(hdr[9] & 0x7F);
    if (hdr[5] & ID3_FOOTER_FLAG)
      size += ID3_FOOTER_LEN;
    if (!skip_bytes(f, size) || fread(hdr, 1, FLAC_MAGIC_LEN, f) != FLAC_MAGIC_LEN)
      goto cleanup;
  }
  if (memcmp(hdr, FLAC_MAGIC, FLAC_MAGIC_LEN) != 0)
    goto cleanup;

  for (int i = 0; i < MAX_BLOCKS; i++) {
    uint8_t bh[BLOCK_HEADER_LEN];
    if (fread(bh, 1, BLOCK_HEADER_LEN, f) != BLOCK_HEADER_LEN)
      break;
    const bool is_last = (bh[0] & BLOCK_LAST_FLAG) != 0;
    const uint8_t type = bh[0] & BLOCK_TYPE_MASK;
    const uint32_t len = ((uint32_t)bh[1] << 16) | ((uint32_t)bh[2] << 8) | bh[3];
    long block_start = ftell(f);
    bool block_ok = true;
    switch (type) {
      case BLOCK_STREAMINFO:
        block_ok = parse_streaminfo(&ctx, len);
        has_streaminfo = block_ok;
        break;
      case BLOCK_VORBIS:
        block_ok = parse_vorbis(&ctx, len);
        break;
      case BLOCK_PICTURE:
        block_ok = parse_picture(&ctx, len);
        break;
      default:
        break;
    }
    if (!block_ok && type == BLOCK_STREAMINFO)
      goto cleanup;
    // Always resynchronise on the declared block length, whatever the parser consumed.
    if (block_start < 0 || fseek(f, block_start + (long)len, SEEK_SET) != 0)
      break;
    if (is_last)
      break;
  }
  ok = has_streaminfo;

cleanup:
  fclose(f);
  return ok;
}

uint32_t ipod_meta_duration_ms(const ipod_meta_t *meta) {
  if (meta == NULL || meta->sample_rate == 0)
    return 0;
  return (uint32_t)((meta->total_frames * MS_PER_SEC) / meta->sample_rate);
}

static bool read_be32(FILE *f, uint32_t *out) {
  uint8_t b[4];
  if (fread(b, 1, sizeof(b), f) != sizeof(b))
    return false;
  *out = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
  return true;
}

static bool read_le32(FILE *f, uint32_t *out) {
  uint8_t b[4];
  if (fread(b, 1, sizeof(b), f) != sizeof(b))
    return false;
  *out = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
  return true;
}

static bool skip_bytes(FILE *f, uint32_t n) {
  return fseek(f, (long)n, SEEK_CUR) == 0;
}

static bool parse_streaminfo(parse_ctx_t *ctx, uint32_t len) {
  uint8_t b[STREAMINFO_LEN];
  if (len < STREAMINFO_LEN || fread(b, 1, STREAMINFO_LEN, ctx->f) != STREAMINFO_LEN)
    return false;
  ipod_meta_t *m = ctx->meta;
  m->sample_rate = ((uint32_t)b[10] << 12) | ((uint32_t)b[11] << 4) | (b[12] >> 4);
  m->channels = (uint8_t)(((b[12] >> 1) & 0x07) + 1);
  m->bits_per_sample = (uint8_t)((((b[12] & 0x01) << 4) | (b[13] >> 4)) + 1);
  m->total_frames = ((uint64_t)(b[13] & 0x0F) << 32) | ((uint64_t)b[14] << 24) |
                    ((uint64_t)b[15] << 16) | ((uint64_t)b[16] << 8) | b[17];
  return m->sample_rate != 0;
}

static bool parse_vorbis(parse_ctx_t *ctx, uint32_t len) {
  uint32_t vendor_len = 0;
  uint32_t count = 0;
  uint32_t used = 0;
  if (!read_le32(ctx->f, &vendor_len) || vendor_len > len || !skip_bytes(ctx->f, vendor_len))
    return false;
  used = 4 + vendor_len;
  if (!read_le32(ctx->f, &count))
    return false;
  used += 4;
  if (count > MAX_COMMENTS)
    count = MAX_COMMENTS;

  char buf[COMMENT_BUF_LEN];
  for (uint32_t i = 0; i < count; i++) {
    uint32_t clen = 0;
    if (!read_le32(ctx->f, &clen))
      return false;
    used += 4;
    if (clen > len - used)
      return false;
    uint32_t take = clen < COMMENT_BUF_LEN - 1 ? clen : COMMENT_BUF_LEN - 1;
    if (fread(buf, 1, take, ctx->f) != take)
      return false;
    buf[take] = '\0';
    if (take < clen && !skip_bytes(ctx->f, clen - take))
      return false;
    used += clen;
    apply_comment(ctx->meta, buf);
  }
  return true;
}

static bool parse_picture(parse_ctx_t *ctx, uint32_t len) {
  uint32_t type = 0;
  uint32_t mime_len = 0;
  uint32_t desc_len = 0;
  uint32_t data_len = 0;
  char mime[IPOD_META_MIME_MAX];
  if (!read_be32(ctx->f, &type) || !read_be32(ctx->f, &mime_len) || mime_len > PICTURE_MAX_FIELD)
    return false;
  uint32_t take = mime_len < sizeof(mime) - 1 ? mime_len : (uint32_t)sizeof(mime) - 1;
  if (fread(mime, 1, take, ctx->f) != take)
    return false;
  mime[take] = '\0';
  if (!skip_bytes(ctx->f, mime_len - take))
    return false;
  if (!read_be32(ctx->f, &desc_len) || desc_len > PICTURE_MAX_FIELD ||
      !skip_bytes(ctx->f, desc_len + PICTURE_FIXED_LEN) || !read_be32(ctx->f, &data_len))
    return false;
  long data_off = ftell(ctx->f);
  if (data_off <= 0 || data_len == 0 || data_len > len)
    return false;

  // Keep the first picture, but let a front cover replace any other type.
  const bool have = ctx->pic_type != PIC_TYPE_UNSET;
  const bool better = type == IPOD_META_PIC_FRONT_COVER && ctx->pic_type != type;
  if (!have || better) {
    ctx->pic_type = type;
    ctx->meta->picture_offset = (uint32_t)data_off;
    ctx->meta->picture_size = data_len;
    ipod_text_copy(ctx->meta->picture_mime, sizeof(ctx->meta->picture_mime), mime);
  }
  return true;
}

static void set_once(char *dst, const char *value) {
  if (dst[0] == '\0')
    ipod_text_copy(dst, IPOD_META_TEXT_MAX, value);
}

static void apply_comment(ipod_meta_t *meta, char *kv) {
  char *eq = strchr(kv, '=');
  if (eq == NULL)
    return;
  *eq = '\0';
  const char *key = kv;
  const char *val = eq + 1;
  while (*val == ' ')
    val++;
  if (*val == '\0')
    return;

  if (ipod_text_equal_ci(key, "TITLE")) {
    set_once(meta->title, val);
  } else if (ipod_text_equal_ci(key, "ARTIST")) {
    set_once(meta->artist, val);
  } else if (ipod_text_equal_ci(key, "ALBUM")) {
    set_once(meta->album, val);
  } else if (ipod_text_equal_ci(key, "ALBUMARTIST") || ipod_text_equal_ci(key, "ALBUM ARTIST") ||
             ipod_text_equal_ci(key, "ALBUM_ARTIST")) {
    set_once(meta->album_artist, val);
  } else if (ipod_text_equal_ci(key, "GENRE")) {
    set_once(meta->genre, val);
  } else if (ipod_text_equal_ci(key, "COMPOSER")) {
    set_once(meta->composer, val);
  } else if (ipod_text_equal_ci(key, "TRACKNUMBER")) {
    meta->track_no = (uint16_t)atoi(val); // "3" or "3/12"
  } else if (ipod_text_equal_ci(key, "DISCNUMBER")) {
    meta->disc_no = (uint16_t)atoi(val);
  } else if (ipod_text_equal_ci(key, "DATE") || ipod_text_equal_ci(key, "YEAR")) {
    if (meta->year == 0)
      meta->year = (uint16_t)atoi(val); // "2004" or "2004-05-01"
  } else if (ipod_text_equal_ci(key, "COMPILATION")) {
    meta->is_compilation = (atoi(val) != 0) || tolower((unsigned char)val[0]) == 't';
  } else if (ipod_text_equal_ci(key, "REPLAYGAIN_TRACK_GAIN")) {
    meta->rg_track_gain_db = parse_gain_db(val);
    meta->has_rg_track = true;
  } else if (ipod_text_equal_ci(key, "REPLAYGAIN_ALBUM_GAIN")) {
    meta->rg_album_gain_db = parse_gain_db(val);
    meta->has_rg_album = true;
  }
}

static float parse_gain_db(const char *s) {
  return strtof(s, NULL); // "-6.48 dB" -> -6.48
}
