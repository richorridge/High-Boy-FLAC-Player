// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// High Boy simulator shim: mirrors the ESP-IDF / TentacleOS declaration of the
// same name so app sources compile unchanged on the host.

#ifndef VFS_CONFIG_H
#define VFS_CONFIG_H

// The simulator redirects this mount point to a host folder (see hb_fs.c).
#define VFS_MOUNT_POINT "/sdcard"

#endif // VFS_CONFIG_H
