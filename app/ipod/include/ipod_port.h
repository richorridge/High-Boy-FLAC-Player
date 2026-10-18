// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// The few platform facts the UI shows, kept behind one small interface:
// port/ipod_port_tentacleos.c on the device, sim/src/hb_port.c on the host.

#ifndef IPOD_PORT_H
#define IPOD_PORT_H

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Battery state of charge 0..100, or -1 when unknown. */
int ipod_port_battery_percent(void);

/** @brief True while the battery is charging. */
bool ipod_port_is_charging(void);

/** @brief Wall-clock time; @p out_is_synced tells if it came from a real source. */
time_t ipod_port_time_now(bool *out_is_synced);

#ifdef __cplusplus
}
#endif

#endif // IPOD_PORT_H
