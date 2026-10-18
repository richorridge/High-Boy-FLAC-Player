// Copyright (c) 2026 High Boy FLAC iPod contributors
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
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hb_sim.h"
#include "vfs_config.h"

#define MOUNT_LEN (sizeof(VFS_MOUNT_POINT) - 1)

FILE *__real_fopen(const char *path, const char *mode);
int __real_fclose(FILE *f);
int __real_stat(const char *path, struct stat *st);
DIR *__real_opendir(const char *path);
int __real_mkdir(const char *path, mode_t mode);
int __real_remove(const char *path);
int __real_rename(const char *from, const char *to);
int __real_access(const char *path, int mode);
int __real_rmdir(const char *path);

FILE *__wrap_fopen(const char *path, const char *mode);
int __wrap_fclose(FILE *f);
int __wrap_stat(const char *path, struct stat *st);
DIR *__wrap_opendir(const char *path);
int __wrap_mkdir(const char *path, mode_t mode);
int __wrap_remove(const char *path);
int __wrap_rename(const char *from, const char *to);
int __wrap_access(const char *path, int mode);
int __wrap_rmdir(const char *path);

#define DEFAULT_MAX_FILES 5 // TentacleOS VFS_MAX_FILES (storage_vfs/include/vfs_config.h)
#define MAX_TRACKED       64

static char s_root[PATH_MAX] = "sdcard";

// Like ESP-IDF's FAT VFS, only VFS_MAX_FILES card files may be open at once;
// one more fopen() fails with ENFILE. HB_MAX_FILES overrides the limit.
static pthread_mutex_t s_files_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_open[MAX_TRACKED];
static int s_max_files = -1;

static int max_files(void) {
  if (s_max_files < 0) {
    const char *v = getenv("HB_MAX_FILES");
    s_max_files = v != NULL ? atoi(v) : DEFAULT_MAX_FILES;
    if (s_max_files > MAX_TRACKED)
      s_max_files = MAX_TRACKED;
  }
  return s_max_files;
}

int hb_fs_open_count(void) {
  pthread_mutex_lock(&s_files_mu);
  int n = 0;
  for (int i = 0; i < MAX_TRACKED; i++)
    n += s_open[i] != NULL;
  pthread_mutex_unlock(&s_files_mu);
  return n;
}

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
  const char *host = map(path, buf, sizeof(buf));
  if (host == path)
    return __real_fopen(path, mode); // not on the card: no limit
  pthread_mutex_lock(&s_files_mu);
  int slot = -1;
  int used = 0;
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (s_open[i] != NULL)
      used++;
    else if (slot < 0)
      slot = i;
  }
  if (used >= max_files() || slot < 0) {
    pthread_mutex_unlock(&s_files_mu);
    errno = ENFILE;
    return NULL;
  }
  FILE *f = __real_fopen(host, mode);
  if (f != NULL)
    s_open[slot] = f;
  pthread_mutex_unlock(&s_files_mu);
  return f;
}

int __wrap_fclose(FILE *f) {
  pthread_mutex_lock(&s_files_mu);
  for (int i = 0; i < MAX_TRACKED; i++) {
    if (s_open[i] == f && f != NULL)
      s_open[i] = NULL;
  }
  pthread_mutex_unlock(&s_files_mu);
  return __real_fclose(f);
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
