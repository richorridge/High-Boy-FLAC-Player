// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_art.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys_prio.h"

#include "ipod_fs.h"
#include "ipod_text.h"
#include "stb_image.h"

#define ART_TASK_STACK     12288
#define ART_TASK_PRIO      SYS_PRIO_BACKGROUND // the player (SERVICE_HI) always wins
#define ART_TASK_CORE      SYS_CORE_RADIO
#define ART_MAX_FILE_BYTES (8u * 1024u * 1024u)
// stb_image holds the whole RGB image (3 B/px); progressive JPEGs also keep
// 16-bit coefficients for every component (another 6 B/px). Stay well inside PSRAM.
#define ART_MAX_PIXELS      (2400u * 1000u)
#define ART_MAX_PIXELS_PROG (1200u * 1000u)
#define ART_PATH_LEN        512
#define REFL_TOP_ALPHA_PCT  40
#define SHUTDOWN_WAIT_MS    2000
#define JPEG_SOF2           0xC2
#define JPEG_SOS            0xDA

static const char *TAG = "IPOD_ART";

// Looked up next to the song when it has no embedded picture. FAT is
// case-insensitive; the capitalised names help on case-sensitive hosts.
static const char *const FOLDER_ART[] = {
    "cover.jpg",
    "folder.jpg",
    "front.jpg",
    "cover.png",
    "folder.png",
    "front.png",
    "Cover.jpg",
    "Folder.jpg",
    "Front.jpg",
    "AlbumArt.jpg",
    "albumart.jpg",
};

typedef struct {
  uint32_t id;
  char path[ART_PATH_LEN];
  uint32_t offset;
  uint32_t size;
  uint16_t box;
  uint16_t refl_h;
  uint16_t bg;
  bool is_pending;
} art_req_t;

static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_exited; ///< given by the task as its very last act
static bool s_running;             ///< touched by the controlling (UI) thread only
static bool s_quit;
static art_req_t s_req;
static uint32_t s_next_id;
static uint32_t s_done_id;
static bool s_done_ready;
static ipod_art_image_t s_done;

static void art_task(void *arg);

esp_err_t ipod_art_start(void) {
  if (s_running)
    return ESP_OK;
  if (s_mu == NULL)
    s_mu = xSemaphoreCreateMutex();
  if (s_wake == NULL)
    s_wake = xSemaphoreCreateBinary();
  if (s_exited == NULL)
    s_exited = xSemaphoreCreateBinary();
  if (s_mu == NULL || s_wake == NULL || s_exited == NULL) {
    ESP_LOGE(TAG, "semaphore alloc failed");
    return ESP_ERR_NO_MEM;
  }
  s_quit = false;
  s_req.is_pending = false;
  s_running = true;
  if (xTaskCreatePinnedToCore(
          art_task, "ipod_art", ART_TASK_STACK, NULL, ART_TASK_PRIO, NULL, ART_TASK_CORE) !=
      pdPASS) {
    ESP_LOGE(TAG, "art task create failed");
    s_running = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}

void ipod_art_shutdown(void) {
  if (!s_running)
    return;
  xSemaphoreTake(s_mu, portMAX_DELAY);
  s_quit = true;
  xSemaphoreGive(s_mu);
  xSemaphoreGive(s_wake);
  if (xSemaphoreTake(s_exited, pdMS_TO_TICKS(SHUTDOWN_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "art task did not stop in time");
    return;
  }
  s_running = false;
  xSemaphoreTake(s_mu, portMAX_DELAY);
  if (s_done_ready)
    ipod_art_image_free(&s_done);
  s_done_ready = false;
  xSemaphoreGive(s_mu);
}

uint32_t ipod_art_request(const char *flac_path,
                          uint32_t pic_offset,
                          uint32_t pic_size,
                          uint16_t box,
                          uint16_t refl_h,
                          uint16_t bg) {
  if (!s_running)
    return 0;
  xSemaphoreTake(s_mu, portMAX_DELAY);
  uint32_t id = ++s_next_id;
  if (id == 0)
    id = ++s_next_id;
  s_req.id = id;
  ipod_text_copy(s_req.path, sizeof(s_req.path), flac_path);
  s_req.offset = pic_offset;
  s_req.size = pic_size;
  s_req.box = box;
  s_req.refl_h = refl_h;
  s_req.bg = bg;
  s_req.is_pending = true;
  xSemaphoreGive(s_mu);
  xSemaphoreGive(s_wake);
  return id;
}

bool ipod_art_take(uint32_t request_id, ipod_art_image_t *out) {
  if (!s_running || request_id == 0)
    return false;
  bool ok = false;
  xSemaphoreTake(s_mu, portMAX_DELAY);
  if (s_done_ready && s_done_id == request_id) {
    *out = s_done;
    memset(&s_done, 0, sizeof(s_done));
    s_done_ready = false;
    ok = true;
  }
  xSemaphoreGive(s_mu);
  return ok;
}

void ipod_art_image_free(ipod_art_image_t *img) {
  if (img == NULL)
    return;
  free(img->pixels);
  free(img->reflection);
  memset(img, 0, sizeof(*img));
}

static uint8_t *read_range(const char *path, uint32_t offset, uint32_t size, uint32_t *out_len) {
  FILE *f = ipod_fopen(path, "rb", IPOD_FS_WAIT_WORKER_MS);
  if (f == NULL)
    return NULL;
  uint8_t *buf = NULL;
  if (size == 0) {
    if (fseek(f, 0, SEEK_END) != 0)
      goto done;
    long end = ftell(f);
    if (end <= 0 || (unsigned long)end > ART_MAX_FILE_BYTES)
      goto done;
    size = (uint32_t)end;
  }
  if (size > ART_MAX_FILE_BYTES || fseek(f, (long)offset, SEEK_SET) != 0)
    goto done;
  buf = malloc(size);
  if (buf == NULL) {
    ESP_LOGE(TAG, "art buffer alloc failed (%u bytes)", (unsigned)size);
    goto done;
  }
  if (fread(buf, 1, size, f) != size) {
    free(buf);
    buf = NULL;
    goto done;
  }
  *out_len = size;
done:
  fclose(f);
  return buf;
}

static uint8_t *load_folder_art(const char *flac_path, uint32_t *out_len) {
  char dir[ART_PATH_LEN];
  char path[ART_PATH_LEN + 16];
  ipod_text_copy(dir, sizeof(dir), flac_path);
  char *slash = strrchr(dir, '/');
  if (slash == NULL)
    return NULL;
  *slash = '\0';
  for (size_t i = 0; i < sizeof(FOLDER_ART) / sizeof(FOLDER_ART[0]); i++) {
    snprintf(path, sizeof(path), "%s/%s", dir, FOLDER_ART[i]);
    uint8_t *buf = read_range(path, 0, 0, out_len);
    if (buf != NULL)
      return buf;
    if (ipod_fs_is_busy(errno))
      return NULL; // the card is busy, not missing the file: don't wait per name
  }
  return NULL;
}

static bool is_progressive_jpeg(const uint8_t *b, uint32_t len) {
  if (len < 4 || b[0] != 0xFF || b[1] != 0xD8)
    return false;
  uint32_t i = 2;
  while (i + 4 <= len) {
    if (b[i] != 0xFF) {
      i++;
      continue;
    }
    uint8_t marker = b[i + 1];
    if (marker == JPEG_SOF2)
      return true;
    if (marker == JPEG_SOS)
      return false;
    uint32_t seg = ((uint32_t)b[i + 2] << 8) | b[i + 3];
    i += 2 + seg;
  }
  return false;
}

static uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static uint16_t blend565(uint16_t c, uint16_t bg, uint32_t alpha_pct) {
  uint32_t r = ((c >> 11) & 0x1F) * alpha_pct + ((bg >> 11) & 0x1F) * (100 - alpha_pct);
  uint32_t g = ((c >> 5) & 0x3F) * alpha_pct + ((bg >> 5) & 0x3F) * (100 - alpha_pct);
  uint32_t b = (c & 0x1F) * alpha_pct + (bg & 0x1F) * (100 - alpha_pct);
  return (uint16_t)(((r / 100) << 11) | ((g / 100) << 5) | (b / 100));
}

bool ipod_art_decode(const char *flac_path,
                     uint32_t pic_offset,
                     uint32_t pic_size,
                     uint16_t box,
                     uint16_t refl_h,
                     uint16_t bg,
                     ipod_art_image_t *out) {
  memset(out, 0, sizeof(*out));
  if (flac_path == NULL || box == 0)
    return false;
  uint32_t len = 0;
  uint8_t *data = pic_size > 0 ? read_range(flac_path, pic_offset, pic_size, &len)
                               : load_folder_art(flac_path, &len);
  if (data == NULL)
    return false;

  int w = 0;
  int h = 0;
  int comp = 0;
  uint8_t *rgb = NULL;
  bool ok = false;
  if (!stbi_info_from_memory(data, (int)len, &w, &h, &comp) || w <= 0 || h <= 0) {
    ESP_LOGW(TAG, "unsupported image in %s", flac_path);
    goto cleanup;
  }
  const uint32_t limit = is_progressive_jpeg(data, len) ? ART_MAX_PIXELS_PROG : ART_MAX_PIXELS;
  if ((uint64_t)w * (uint64_t)h > limit) {
    ESP_LOGW(TAG, "cover %dx%d too large to decode", w, h);
    goto cleanup;
  }
  rgb = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 3);
  if (rgb == NULL) {
    ESP_LOGW(TAG, "decode failed for %s", flac_path);
    goto cleanup;
  }

  // Fit inside box x box keeping the aspect ratio.
  uint32_t ow = box;
  uint32_t oh = box;
  if (w >= h)
    oh = (uint32_t)(((uint64_t)h * box + (uint32_t)w / 2) / (uint32_t)w);
  else
    ow = (uint32_t)(((uint64_t)w * box + (uint32_t)h / 2) / (uint32_t)h);
  if (ow == 0)
    ow = 1;
  if (oh == 0)
    oh = 1;
  if (refl_h > oh)
    refl_h = (uint16_t)oh;

  out->pixels = malloc((size_t)ow * oh * sizeof(uint16_t));
  if (refl_h > 0)
    out->reflection = malloc((size_t)ow * refl_h * sizeof(uint16_t));
  if (out->pixels == NULL || (refl_h > 0 && out->reflection == NULL)) {
    ESP_LOGE(TAG, "thumbnail alloc failed");
    ipod_art_image_free(out);
    goto cleanup;
  }
  out->w = (uint16_t)ow;
  out->h = (uint16_t)oh;
  out->refl_h = refl_h;

  // Box filter: average every source pixel that lands in each output pixel.
  for (uint32_t oy = 0; oy < oh; oy++) {
    uint32_t y0 = oy * (uint32_t)h / oh;
    uint32_t y1 = (oy + 1) * (uint32_t)h / oh;
    if (y1 <= y0)
      y1 = y0 + 1;
    for (uint32_t ox = 0; ox < ow; ox++) {
      uint32_t x0 = ox * (uint32_t)w / ow;
      uint32_t x1 = (ox + 1) * (uint32_t)w / ow;
      if (x1 <= x0)
        x1 = x0 + 1;
      uint32_t r = 0;
      uint32_t g = 0;
      uint32_t b = 0;
      for (uint32_t y = y0; y < y1; y++) {
        const uint8_t *p = rgb + ((size_t)y * (uint32_t)w + x0) * 3;
        for (uint32_t x = x0; x < x1; x++, p += 3) {
          r += p[0];
          g += p[1];
          b += p[2];
        }
      }
      uint32_t n = (y1 - y0) * (x1 - x0);
      out->pixels[oy * ow + ox] = rgb565(r / n, g / n, b / n);
    }
  }
  for (uint32_t k = 0; k < refl_h; k++) {
    uint32_t alpha = REFL_TOP_ALPHA_PCT * (refl_h - k) / refl_h;
    const uint16_t *src = out->pixels + (oh - 1 - k) * ow;
    for (uint32_t x = 0; x < ow; x++)
      out->reflection[k * ow + x] = blend565(src[x], bg, alpha);
  }
  ok = true;

cleanup:
  stbi_image_free(rgb);
  free(data);
  return ok;
}

static void art_task(void *arg) {
  (void)arg;
  art_req_t *req = malloc(sizeof(*req));
  if (req == NULL) {
    ESP_LOGE(TAG, "request alloc failed");
    // Keep draining wake-ups so shutdown still completes.
    for (;;) {
      xSemaphoreTake(s_wake, portMAX_DELAY);
      xSemaphoreTake(s_mu, portMAX_DELAY);
      const bool quit = s_quit;
      s_req.is_pending = false;
      xSemaphoreGive(s_mu);
      if (quit)
        break;
    }
    xSemaphoreGive(s_exited);
    vTaskDelete(NULL);
    return;
  }
  for (;;) {
    xSemaphoreTake(s_wake, portMAX_DELAY);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    const bool quit = s_quit;
    const bool have = s_req.is_pending;
    if (have) {
      *req = s_req;
      s_req.is_pending = false;
    }
    xSemaphoreGive(s_mu);
    if (quit)
      break;
    if (!have)
      continue;

    ipod_art_image_t img;
    if (!ipod_art_decode(req->path, req->offset, req->size, req->box, req->refl_h, req->bg, &img))
      memset(&img, 0, sizeof(img)); // "no art" is a valid answer too

    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_done_ready)
      ipod_art_image_free(&s_done); // never collected: superseded
    s_done = img;
    s_done_id = req->id;
    s_done_ready = true;
    xSemaphoreGive(s_mu);
  }
  free(req);
  xSemaphoreGive(s_exited);
  vTaskDelete(NULL);
}
