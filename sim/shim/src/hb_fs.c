// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// Redirect the device's /sdcard mount point to a host folder. The simulator
// links with -Wl,--wrap=<fn> for each function below (as TentacleOS's own HLE
// does), so app code and dr_flac keep using real device paths.

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hb_sim.h"
#include "vfs_config.h"

#define MOUNT_LEN (sizeof(VFS_MOUNT_POINT) - 1)

FILE *__real_fopen(const char *path, const char *mode);
int __real_stat(const char *path, struct stat *st);
DIR *__real_opendir(const char *path);
int __real_mkdir(const char *path, mode_t mode);
int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
int __real_access(const char *path, int mode);
int __real_rmdir(const char *path);

FILE *__wrap_fopen(const char *path, const char *mode);
int __wrap_stat(const char *path, struct stat *st);
DIR *__wrap_opendir(const char *path);
int __wrap_mkdir(const char *path, mode_t mode);
int __wrap_remove(const char *path);
int __wrap_rename(const char *from, const char *to);
int __wrap_access(const char *path, int mode);
int __wrap_rmdir(const char *path);

static char s_root[PATH_MAX] = "sdcard";

void hb_fs_set_sd_root(const char *host_dir) {
  snprintf(s_root, sizeof(s_root), "%s", host_dir);
  size_t n = strlen(s_root);
  while (n > 1 && s_root[n - 1] == '/')
    s_root[--n] = '\0';
}

// Returns the host path for a device path; anything outside /sdcard is untouched.
static const char *map(const char *path, char *buf, size_t len) {
  if (path == NULL || strncmp(path, VFS_MOUNT_POINT, MOUNT_LEN) != 0)
    return path;
  if (path[MOUNT_LEN] != '\0' && path[MOUNT_LEN] != '/')
    return path; // e.g. "/sdcard2"
  snprintf(buf, len, "%s%s", s_root, path + MOUNT_LEN);
  return buf;
}

FILE *__wrap_fopen(const char *path, const char *mode) {
  char buf[PATH_MAX];
  return __real_fopen(map(path, buf, sizeof(buf)), mode);
}

int __wrap_stat(const char *path, struct stat *st) {
  char buf[PATH_MAX];
  return __real_stat(map(path, buf, sizeof(buf)), st);
}

DIR *__wrap_opendir(const char *path) {
  char buf[PATH_MAX];
  return __real_opendir(map(path, buf, sizeof(buf)));
}

int __wrap_mkdir(const char *path, mode_t mode) {
  char buf[PATH_MAX];
  return __real_mkdir(map(path, buf, sizeof(buf)), mode);
}

int __wrap_remove(const char *path) {
  char buf[PATH_MAX];
  return __real_remove(map(path, buf, sizeof(buf)));
}

int __wrap_rename(const char *from, const char *to) {
  char a[PATH_MAX];
  char b[PATH_MAX];
  return __real_rename(map(from, a, sizeof(a)), map(to, b, sizeof(b)));
}

int __wrap_access(const char *path, int mode) {
  char buf[PATH_MAX];
  return __real_access(map(path, buf, sizeof(buf)), mode);
}

int __wrap_rmdir(const char *path) {
  char buf[PATH_MAX];
  return __real_rmdir(map(path, buf, sizeof(buf)));
}
