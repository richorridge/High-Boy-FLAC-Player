// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// TentacleOS implementation of ipod_port.h (device build only).

#include "ipod_port.h"

#include "battery_service.h"
#include "sys_time.h"

int ipod_port_battery_percent(void) {
  return battery_service_soc();
}

bool ipod_port_is_charging(void) {
  battery_snapshot_t snap;
  return battery_service_get(&snap) && snap.charging;
}

time_t ipod_port_time_now(bool *out_is_synced) {
  if (out_is_synced != NULL)
    *out_is_synced = sys_time_state() == SYS_TIME_STATE_SYNCED;
  return sys_time_now();
}
