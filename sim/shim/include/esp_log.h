// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef ESP_LOG_H
#define ESP_LOG_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_LOG_NONE = 0,
  ESP_LOG_ERROR,
  ESP_LOG_WARN,
  ESP_LOG_INFO,
  ESP_LOG_DEBUG,
  ESP_LOG_VERBOSE,
} esp_log_level_t;

void hb_sim_log(esp_log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, ...) hb_sim_log(ESP_LOG_ERROR, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) hb_sim_log(ESP_LOG_WARN, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) hb_sim_log(ESP_LOG_INFO, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) hb_sim_log(ESP_LOG_DEBUG, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) hb_sim_log(ESP_LOG_VERBOSE, tag, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // ESP_LOG_H
