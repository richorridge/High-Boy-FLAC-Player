// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_fs.h"

#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RETRY_PERIOD_MS 20

static const char *TAG = "IPOD_FS";

bool ipod_fs_is_busy(int err) {
  return err == ENFILE || err == EMFILE;
}

FILE *ipod_fopen(const char *path, const char *mode, uint32_t wait_ms) {
  uint32_t waited = 0;
  for (;;) {
    errno = 0;
    FILE *f = fopen(path, mode);
    if (f != NULL)
      return f;
    const int err = errno;
    if (!ipod_fs_is_busy(err) || waited >= wait_ms) {
      if (ipod_fs_is_busy(err))
        ESP_LOGW(TAG, "no free SD file slot for %s after %u ms", path, (unsigned)waited);
      errno = err;
      return NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(RETRY_PERIOD_MS));
    waited += RETRY_PERIOD_MS;
  }
}
