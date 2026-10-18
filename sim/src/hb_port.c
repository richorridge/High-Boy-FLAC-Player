// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// Simulator implementation of ipod_port.h. HB_BATTERY=0..100 sets the
// battery level, HB_CHARGING=1 shows charging, HB_TIME=<epoch> freezes the
// clock (handy for reproducible screenshots).

#include <stdlib.h>

#include "ipod_port.h"

int ipod_port_battery_percent(void) {
  const char *v = getenv("HB_BATTERY");
  return v != NULL ? atoi(v) : 80;
}

bool ipod_port_is_charging(void) {
  const char *v = getenv("HB_CHARGING");
  return v != NULL && atoi(v) != 0;
}

time_t ipod_port_time_now(bool *out_is_synced) {
  if (out_is_synced != NULL)
    *out_is_synced = true;
  const char *v = getenv("HB_TIME");
  return v != NULL ? (time_t)atoll(v) : time(NULL);
}
