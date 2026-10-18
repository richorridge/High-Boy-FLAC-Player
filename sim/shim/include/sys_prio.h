// Copyright (c) 2026 High Boy FLAC iPod contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef SYS_PRIO_H
#define SYS_PRIO_H

#include "freertos/FreeRTOS.h"

#define SYS_CORE_UI    1
#define SYS_CORE_RADIO 0
#define SYS_CORE_ANY   tskNO_AFFINITY

#define SYS_PRIO_REALTIME      10
#define SYS_PRIO_RENDER        6
#define SYS_PRIO_SERVICE_HI    5
#define SYS_PRIO_SERVICE_LO    4
#define SYS_PRIO_BACKGROUND    3
#define SYS_PRIO_BACKGROUND_LO 2
#define SYS_PRIO_MONITOR       1

#endif // SYS_PRIO_H
