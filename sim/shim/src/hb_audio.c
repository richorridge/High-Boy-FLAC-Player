// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.
//
// audio_i2s for the simulator. Same API and volume law as the High Boy's
// driver (16-bit mono, square-law digital volume), with selectable output:
//   HB_AUDIO=sdl   play through the host sound card (default when available)
//   HB_AUDIO=null  discard, but pace writes in real time like the I2S DMA
//   HB_AUDIO=fast  discard without pacing (tests decode as fast as possible)
//   HB_AUDIO_CAPTURE=out.wav  also record exactly what the speaker receives

#define _GNU_SOURCE
#include "audio_i2s.h"
#include "hb_sim.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef HB_WITH_SDL
#include <SDL2/SDL.h>
#endif

#include "esp_log.h"

#define DEFAULT_RATE_HZ 44100u
#define DMA_LEAD_MS     46 // the device keeps 8 x 256 frames queued
#define SDL_QUEUE_MS    80
#define SDL_POLL_US     2000
#define CLICK_MS        30
#define CLICK_HZ        2000
#define CLICK_AMPLITUDE 9000
#define WAV_HEADER_LEN  44
#define US_PER_SEC      1000000LL
#define NS_PER_US       1000LL

typedef enum {
  OUT_NULL = 0,
  OUT_FAST,
  OUT_SDL,
} out_mode_t;

static const char *TAG = "HB_AUDIO";

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static bool s_inited;
static out_mode_t s_mode;
static float s_play_vol = 1.0f;
static uint8_t s_vol_pct = 100;
static bool s_streaming;
static uint32_t s_rate = DEFAULT_RATE_HZ;
static int64_t s_pace_start_us;
static int64_t s_paced_us;
static FILE *s_capture;
static FILE *s_capture_rates;
static uint32_t s_capture_rate;
static uint64_t s_capture_samples;
static hb_audio_stats_t s_stats;
#ifdef HB_WITH_SDL
static SDL_AudioDeviceID s_dev;
static uint32_t s_dev_rate;
#endif

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * US_PER_SEC + ts.tv_nsec / NS_PER_US;
}

static void put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void write_wav_header(FILE *f, uint32_t rate, uint64_t samples) {
  uint8_t h[WAV_HEADER_LEN];
  uint32_t data = (uint32_t)(samples * 2);
  memcpy(h, "RIFF", 4);
  put_le32(h + 4, 36 + data);
  memcpy(h + 8, "WAVEfmt ", 8);
  put_le32(h + 16, 16);
  h[20] = 1; // PCM
  h[21] = 0;
  h[22] = 1; // mono
  h[23] = 0;
  put_le32(h + 24, rate);
  put_le32(h + 28, rate * 2);
  h[32] = 2;
  h[33] = 0;
  h[34] = 16;
  h[35] = 0;
  memcpy(h + 36, "data", 4);
  put_le32(h + 40, data);
  fseek(f, 0, SEEK_SET);
  fwrite(h, 1, sizeof(h), f);
  fseek(f, 0, SEEK_END);
}

static void init_locked(void) {
  if (s_inited)
    return;
  s_inited = true;
  const char *mode = getenv("HB_AUDIO");
  s_mode = OUT_NULL;
  if (mode != NULL && strcmp(mode, "fast") == 0)
    s_mode = OUT_FAST;
#ifdef HB_WITH_SDL
  if (mode == NULL || strcmp(mode, "sdl") == 0) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
      s_mode = OUT_SDL;
    } else {
      ESP_LOGW(TAG, "no audio device (%s): running silent", SDL_GetError());
    }
  }
#endif
  const char *cap = getenv("HB_AUDIO_CAPTURE");
  if (cap != NULL && cap[0] != '\0') {
    s_capture = fopen(cap, "w+b");
    char rates[1024];
    snprintf(rates, sizeof(rates), "%s.rates", cap);
    s_capture_rates = fopen(rates, "w");
    if (s_capture != NULL) {
      uint8_t zero[WAV_HEADER_LEN] = {0};
      fwrite(zero, 1, sizeof(zero), s_capture);
    }
  }
}

#ifdef HB_WITH_SDL
static void sdl_open_locked(uint32_t rate) {
  if (s_dev != 0 && s_dev_rate == rate)
    return;
  if (s_dev != 0) {
    SDL_CloseAudioDevice(s_dev);
    s_dev = 0;
  }
  SDL_AudioSpec want = {0};
  want.freq = (int)rate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;
  s_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0); // SDL converts as needed
  if (s_dev == 0) {
    ESP_LOGW(TAG, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
    s_mode = OUT_NULL;
    return;
  }
  s_dev_rate = rate;
  SDL_PauseAudioDevice(s_dev, 0);
}
#endif

static void capture_locked(const int16_t *pcm, int n, uint32_t rate) {
  if (s_capture == NULL)
    return;
  if (s_capture_rate != rate) {
    if (s_capture_rates != NULL) {
      fprintf(s_capture_rates, "%llu %u\n", (unsigned long long)s_capture_samples, (unsigned)rate);
      fflush(s_capture_rates);
    }
    if (s_capture_rate == 0)
      s_capture_rate = rate;
  }
  fwrite(pcm, sizeof(int16_t), (size_t)n, s_capture);
  s_capture_samples += (uint64_t)n;
  write_wav_header(s_capture, s_capture_rate, s_capture_samples);
  fflush(s_capture);
}

// Output n samples at rate and block like the I2S DMA would.
static void emit_locked(const int16_t *pcm, int n, uint32_t rate) {
  capture_locked(pcm, n, rate);
  s_stats.samples_out += (uint64_t)n;
  switch (s_mode) {
#ifdef HB_WITH_SDL
    case OUT_SDL: {
      sdl_open_locked(rate);
      if (s_dev == 0)
        break;
      SDL_QueueAudio(s_dev, pcm, (uint32_t)n * sizeof(int16_t));
      const uint32_t limit = rate * sizeof(int16_t) * SDL_QUEUE_MS / 1000;
      pthread_mutex_unlock(&s_mu);
      while (SDL_GetQueuedAudioSize(s_dev) > limit)
        usleep(SDL_POLL_US);
      pthread_mutex_lock(&s_mu);
      return;
    }
#endif
    case OUT_NULL: {
      int64_t now = now_us();
      if (s_pace_start_us == 0 || now - (s_pace_start_us + s_paced_us) > DMA_LEAD_MS * 1000) {
        s_pace_start_us = now; // first write, or we fell behind: restart the clock
        s_paced_us = 0;
      }
      s_paced_us += (int64_t)n * US_PER_SEC / rate;
      int64_t due = s_pace_start_us + s_paced_us - DMA_LEAD_MS * 1000;
      pthread_mutex_unlock(&s_mu);
      int64_t wait = due - now_us();
      if (wait > 0)
        usleep((useconds_t)wait);
      pthread_mutex_lock(&s_mu);
      return;
    }
    default:
      return;
  }
}

esp_err_t audio_i2s_init(void) {
  pthread_mutex_lock(&s_mu);
  init_locked();
  pthread_mutex_unlock(&s_mu);
  return ESP_OK;
}

void audio_i2s_set_volume(uint8_t pct) {
  if (pct > 100)
    pct = 100;
  float n = pct / 100.0f;
  pthread_mutex_lock(&s_mu);
  s_vol_pct = pct;
  s_play_vol = n * n;
  pthread_mutex_unlock(&s_mu);
}

esp_err_t audio_i2s_stream_start(uint32_t sample_rate) {
  pthread_mutex_lock(&s_mu);
  init_locked();
  s_streaming = true;
  s_rate = sample_rate;
  s_pace_start_us = 0;
  s_stats.stream_starts++;
  s_stats.last_rate = sample_rate;
  pthread_mutex_unlock(&s_mu);
  return ESP_OK;
}

int audio_i2s_stream_write(const int16_t *pcm, int n_samples) {
  if (pcm == NULL || n_samples <= 0)
    return -1;
  int16_t *buf = malloc((size_t)n_samples * sizeof(int16_t));
  if (buf == NULL)
    return 0;
  pthread_mutex_lock(&s_mu);
  if (!s_streaming) {
    pthread_mutex_unlock(&s_mu);
    free(buf);
    return -1;
  }
  for (int i = 0; i < n_samples; i++) {
    int v = (int)(pcm[i] * s_play_vol); // identical to the device driver
    if (v > 32767)
      v = 32767;
    else if (v < -32768)
      v = -32768;
    buf[i] = (int16_t)v;
  }
  emit_locked(buf, n_samples, s_rate);
  pthread_mutex_unlock(&s_mu);
  free(buf);
  return n_samples;
}

void audio_i2s_stream_stop(void) {
  pthread_mutex_lock(&s_mu);
  if (s_streaming)
    s_stats.stream_stops++;
  s_streaming = false;
  pthread_mutex_unlock(&s_mu);
}

void audio_click(void) {
  pthread_mutex_lock(&s_mu);
  init_locked();
  s_stats.ui_clicks++;
  if (s_streaming) { // the real driver drops UI sounds while streaming
    pthread_mutex_unlock(&s_mu);
    return;
  }
#ifdef HB_WITH_SDL
  if (s_mode == OUT_SDL) {
    const uint32_t rate = DEFAULT_RATE_HZ;
    const int n = (int)(rate * CLICK_MS / 1000);
    int16_t *click = malloc((size_t)n * sizeof(int16_t));
    if (click != NULL) {
      for (int i = 0; i < n; i++) {
        float env = 1.0f - (float)i / (float)n;
        int phase = (int)((int64_t)i * CLICK_HZ * 2 / rate) & 1;
        click[i] = (int16_t)((phase ? CLICK_AMPLITUDE : -CLICK_AMPLITUDE) * env * env * s_play_vol);
      }
      sdl_open_locked(rate);
      if (s_dev != 0)
        SDL_QueueAudio(s_dev, click, (uint32_t)n * sizeof(int16_t));
      free(click);
    }
  }
#endif
  pthread_mutex_unlock(&s_mu);
}

void hb_audio_get_stats(hb_audio_stats_t *out) {
  pthread_mutex_lock(&s_mu);
  *out = s_stats;
  out->streaming = s_streaming;
  out->volume_pct = s_vol_pct;
  pthread_mutex_unlock(&s_mu);
}
