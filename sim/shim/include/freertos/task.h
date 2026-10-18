// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hb_task *TaskHandle_t;
typedef void (*TaskFunction_t)(void *arg);

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn,
                                   const char *name,
                                   uint32_t stack_depth,
                                   void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_handle,
                                   BaseType_t core_id);
void vTaskDelete(TaskHandle_t task);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#ifdef __cplusplus
}
#endif

#endif // FREERTOS_TASK_H
