// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef FREERTOS_SEMPHR_H
#define FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hb_sem *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

#ifdef __cplusplus
}
#endif

#endif // FREERTOS_SEMPHR_H
