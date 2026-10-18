// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef FREERTOS_H
#define FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define configTICK_RATE_HZ   1000
#define portTICK_PERIOD_MS   (1000 / configTICK_RATE_HZ)
#define portMAX_DELAY        ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms)    ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))
#define pdTICKS_TO_MS(ticks) ((uint32_t)(((uint64_t)(ticks) * 1000u) / configTICK_RATE_HZ))
#define pdTRUE               1
#define pdFALSE              0
#define pdPASS               pdTRUE
#define pdFAIL               pdFALSE
#define tskNO_AFFINITY       0x7FFFFFFF

#endif // FREERTOS_H
