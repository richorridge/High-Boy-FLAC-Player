// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// FreeRTOS subset on pthreads, plus the ESP-IDF log sink.

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NS_PER_MS  1000000L
#define NS_PER_SEC 1000000000L
#define MS_PER_SEC 1000L

struct hb_task {
  pthread_t thread;
  TaskFunction_t fn;
  void *arg;
  char name[16];
};

struct hb_sem {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int count;
  int max;
};

static esp_log_level_t s_log_level = ESP_LOG_INFO;
static pthread_once_t s_log_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t s_log_mu = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_boot;
static pthread_once_t s_boot_once = PTHREAD_ONCE_INIT;

static void boot_init(void) {
  clock_gettime(CLOCK_MONOTONIC, &s_boot);
}

static void log_init(void) {
  const char *lvl = getenv("HB_LOG");
  if (lvl == NULL)
    return;
  if (strcmp(lvl, "none") == 0)
    s_log_level = ESP_LOG_NONE;
  else if (strcmp(lvl, "error") == 0)
    s_log_level = ESP_LOG_ERROR;
  else if (strcmp(lvl, "warn") == 0)
    s_log_level = ESP_LOG_WARN;
  else if (strcmp(lvl, "debug") == 0)
    s_log_level = ESP_LOG_DEBUG;
  else if (strcmp(lvl, "verbose") == 0)
    s_log_level = ESP_LOG_VERBOSE;
}

void hb_sim_log(esp_log_level_t level, const char *tag, const char *fmt, ...) {
  pthread_once(&s_log_once, log_init);
  if (level > s_log_level)
    return;
  static const char LETTERS[] = "NEWIDV";
  pthread_mutex_lock(&s_log_mu);
  fprintf(stderr, "%c (%u) %s: ", LETTERS[level], (unsigned)xTaskGetTickCount(), tag);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  pthread_mutex_unlock(&s_log_mu);
}

static void *task_trampoline(void *p) {
  struct hb_task *t = p;
  pthread_setname_np(pthread_self(), t->name);
  t->fn(t->arg);
  free(t); // a task function that simply returns is treated like vTaskDelete(NULL)
  return NULL;
}

static __thread struct hb_task *s_self;

static void *task_entry(void *p) {
  s_self = p;
  return task_trampoline(p);
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn,
                                   const char *name,
                                   uint32_t stack_depth,
                                   void *arg,
                                   UBaseType_t priority,
                                   TaskHandle_t *out_handle,
                                   BaseType_t core_id) {
  (void)stack_depth;
  (void)priority;
  (void)core_id;
  struct hb_task *t = calloc(1, sizeof(*t));
  if (t == NULL)
    return pdFAIL;
  t->fn = fn;
  t->arg = arg;
  snprintf(t->name, sizeof(t->name), "%s", name != NULL ? name : "task");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&t->thread, &attr, task_entry, t);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    free(t);
    return pdFAIL;
  }
  if (out_handle != NULL)
    *out_handle = t;
  return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
  if (task == NULL || task == s_self) {
    free(s_self);
    s_self = NULL;
    pthread_exit(NULL);
  }
  // Deleting another task is not used by the app (it is unsafe on the device too).
}

void vTaskDelay(TickType_t ticks) {
  struct timespec ts = {.tv_sec = (time_t)(ticks / MS_PER_SEC),
                        .tv_nsec = (long)(ticks % MS_PER_SEC) * NS_PER_MS};
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
  }
}

TickType_t xTaskGetTickCount(void) {
  pthread_once(&s_boot_once, boot_init);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t ms = (int64_t)(now.tv_sec - s_boot.tv_sec) * MS_PER_SEC +
               (now.tv_nsec - s_boot.tv_nsec) / NS_PER_MS;
  return (TickType_t)ms;
}

static SemaphoreHandle_t sem_create(int initial, int max) {
  struct hb_sem *s = calloc(1, sizeof(*s));
  if (s == NULL)
    return NULL;
  pthread_mutex_init(&s->mu, NULL);
  pthread_condattr_t ca;
  pthread_condattr_init(&ca);
  pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
  pthread_cond_init(&s->cv, &ca);
  pthread_condattr_destroy(&ca);
  s->count = initial;
  s->max = max;
  return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  return sem_create(1, 1);
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  return sem_create(0, 1);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  if (s == NULL)
    return pdFALSE;
  pthread_mutex_lock(&s->mu);
  if (ticks == portMAX_DELAY) {
    while (s->count == 0)
      pthread_cond_wait(&s->cv, &s->mu);
  } else {
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += (time_t)(ticks / MS_PER_SEC);
    dl.tv_nsec += (long)(ticks % MS_PER_SEC) * NS_PER_MS;
    if (dl.tv_nsec >= NS_PER_SEC) {
      dl.tv_sec++;
      dl.tv_nsec -= NS_PER_SEC;
    }
    while (s->count == 0) {
      if (pthread_cond_timedwait(&s->cv, &s->mu, &dl) == ETIMEDOUT)
        break;
    }
  }
  BaseType_t ok = pdFALSE;
  if (s->count > 0) {
    s->count--;
    ok = pdTRUE;
  }
  pthread_mutex_unlock(&s->mu);
  return ok;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
  if (s == NULL)
    return pdFALSE;
  pthread_mutex_lock(&s->mu);
  BaseType_t ok = pdFALSE;
  if (s->count < s->max) {
    s->count++;
    ok = pdTRUE;
    pthread_cond_signal(&s->cv);
  }
  pthread_mutex_unlock(&s->mu);
  return ok;
}

void vSemaphoreDelete(SemaphoreHandle_t s) {
  if (s == NULL)
    return;
  pthread_mutex_destroy(&s->mu);
  pthread_cond_destroy(&s->cv);
  free(s);
}
