// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_player.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys_prio.h"

#include "audio_i2s.h"
#include "dr_flac.h"
#include "ipod_dsp.h"

#define PLAYER_TASK_STACK 16384
#define PLAYER_TASK_PRIO  SYS_PRIO_SERVICE_HI
// Core 0: on the UI core a full-frame LVGL redraw (SYS_PRIO_RENDER) could starve
// the ~46 ms I2S DMA queue; the radios are idle while the iPod app runs.
#define PLAYER_TASK_CORE  SYS_CORE_RADIO
#define CHUNK_OUT_SAMPLES 1024 // ~23 ms at 44.1 kHz: seek/pause latency
#define MAX_DECIM         4
#define MAX_CHANNELS      8
#define PATH_LEN          512
#define IDLE_WAIT_MS      100
#define PAUSE_SILENCE_MS  2000 // keep the amp fed briefly, then power the stream down
#define SHUTDOWN_WAIT_MS  1500
#define MS_PER_SEC        1000u
#define CDB_PER_DB        100.0f

static const char *TAG = "IPOD_PLAYER";

static SemaphoreHandle_t s_mu;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_exited; ///< given by the task as its very last act
static TaskHandle_t s_task;
static bool s_task_running; ///< touched by the controlling (UI) thread only

// Everything below is guarded by s_mu.
static bool s_quit;
static ipod_library_t *s_lib;
static ipod_queue_t s_queue;
static ipod_play_state_t s_state;
static bool s_cmd_open;
static bool s_cmd_stop;
static bool s_cmd_seek;
static uint32_t s_seek_ms;
static bool s_has_track;
static uint32_t s_cur_track;
static uint32_t s_pos_ms;
static uint32_t s_dur_ms;
static uint32_t s_track_seq;
static uint32_t s_end_seq;
static uint32_t s_error_seq;
static int s_eq;
static bool s_sound_check;
static bool s_dsp_dirty;
static bool s_stats_dirty;
static uint32_t s_play_seq;
static uint32_t s_stream_rate; ///< 0 while the I2S stream is closed
static atomic_bool s_click;    ///< set by the UI thread, consumed by the decoder

static void player_task(void *arg);

static void lock(void) {
  xSemaphoreTake(s_mu, portMAX_DELAY);
}

static void unlock(void) {
  xSemaphoreGive(s_mu);
}

static void wake(void) {
  xSemaphoreGive(s_wake);
}

esp_err_t ipod_player_start(ipod_library_t *lib) {
  if (s_task_running) {
    ipod_player_set_library(lib);
    return ESP_OK;
  }
  if (s_mu == NULL)
    s_mu = xSemaphoreCreateMutex();
  if (s_wake == NULL)
    s_wake = xSemaphoreCreateBinary();
  if (s_exited == NULL)
    s_exited = xSemaphoreCreateBinary();
  if (s_mu == NULL || s_wake == NULL || s_exited == NULL) {
    ESP_LOGE(TAG, "semaphore alloc failed");
    return ESP_ERR_NO_MEM;
  }
  lock();
  s_quit = false;
  s_lib = lib;
  ipod_queue_init(&s_queue, xTaskGetTickCount() | 1u);
  s_state = IPOD_PLAYER_STOPPED;
  s_cmd_open = false;
  s_cmd_stop = false;
  s_cmd_seek = false;
  s_has_track = false;
  s_pos_ms = 0;
  s_dur_ms = 0;
  s_stream_rate = 0;
  s_play_seq = ipod_lib_max_play_seq(lib);
  unlock();

  s_task_running = true;
  if (xTaskCreatePinnedToCore(player_task,
                              "ipod_play",
                              PLAYER_TASK_STACK,
                              NULL,
                              PLAYER_TASK_PRIO,
                              &s_task,
                              PLAYER_TASK_CORE) != pdPASS) {
    ESP_LOGE(TAG, "player task create failed");
    s_task_running = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}

void ipod_player_shutdown(void) {
  if (!s_task_running)
    return;
  lock();
  s_quit = true;
  unlock();
  wake();
  if (xSemaphoreTake(s_exited, pdMS_TO_TICKS(SHUTDOWN_WAIT_MS)) != pdTRUE) {
    ESP_LOGE(TAG, "player task did not stop in time");
    return; // leave the state alone rather than free what the task still uses
  }
  s_task_running = false;
  lock();
  ipod_queue_free(&s_queue);
  s_lib = NULL;
  s_has_track = false;
  unlock();
}

void ipod_player_set_library(ipod_library_t *lib) {
  if (s_mu == NULL)
    return;
  lock();
  ipod_queue_free(&s_queue);
  s_lib = lib;
  s_state = IPOD_PLAYER_STOPPED;
  s_has_track = false;
  s_cmd_stop = true;
  s_cmd_open = false;
  s_play_seq = ipod_lib_max_play_seq(lib);
  unlock();
  wake();
}

void ipod_player_play_list(const uint32_t *tracks,
                           const uint32_t *groups,
                           size_t count,
                           size_t start) {
  if (s_mu == NULL)
    return;
  lock();
  if (ipod_queue_set(&s_queue, tracks, groups, count, start)) {
    s_state = IPOD_PLAYER_PLAYING;
    s_cmd_open = true;
    s_cmd_seek = false;
    s_pos_ms = 0;
  }
  unlock();
  wake();
}

void ipod_player_set_paused(bool paused) {
  if (s_mu == NULL)
    return;
  lock();
  if (s_state == IPOD_PLAYER_STOPPED) {
    // Play/Pause after the list ended restarts the current list position.
    if (!paused && s_queue.count > 0) {
      s_state = IPOD_PLAYER_PLAYING;
      s_cmd_open = true;
    }
  } else {
    s_state = paused ? IPOD_PLAYER_PAUSED : IPOD_PLAYER_PLAYING;
  }
  unlock();
  wake();
}

void ipod_player_toggle_pause(void) {
  ipod_player_status_t st;
  ipod_player_get_status(&st);
  ipod_player_set_paused(st.state == IPOD_PLAYER_PLAYING);
}

static void stop_locked(void) {
  s_state = IPOD_PLAYER_STOPPED;
  s_has_track = false;
  s_cmd_stop = true;
  s_cmd_open = false;
  s_cmd_seek = false;
  s_pos_ms = 0;
}

void ipod_player_stop(void) {
  if (s_mu == NULL)
    return;
  lock();
  stop_locked();
  unlock();
  wake();
}

static void next_locked(void) {
  if (s_queue.count == 0)
    return;
  if (ipod_queue_next(&s_queue, true)) {
    s_cmd_open = true;
    s_cmd_seek = false;
    s_pos_ms = 0;
  } else {
    stop_locked();
    s_end_seq++;
  }
}

void ipod_player_next(void) {
  if (s_mu == NULL)
    return;
  lock();
  next_locked();
  unlock();
  wake();
}

void ipod_player_prev(void) {
  if (s_mu == NULL)
    return;
  lock();
  if (s_queue.count > 0) {
    if (s_has_track && s_pos_ms > IPOD_PLAYER_RESTART_MS) {
      s_cmd_seek = true;
      s_seek_ms = 0;
      s_pos_ms = 0;
    } else {
      ipod_queue_prev(&s_queue);
      s_cmd_open = true;
      s_cmd_seek = false;
      s_pos_ms = 0;
    }
    if (s_state == IPOD_PLAYER_STOPPED)
      s_state = IPOD_PLAYER_PLAYING;
  }
  unlock();
  wake();
}

void ipod_player_seek_to(uint32_t ms) {
  if (s_mu == NULL)
    return;
  lock();
  if (s_has_track) {
    if (s_dur_ms > 0 && ms >= s_dur_ms)
      ms = s_dur_ms - 1;
    s_cmd_seek = true;
    s_seek_ms = ms;
    s_pos_ms = ms; // show the new position right away
  }
  unlock();
  wake();
}

void ipod_player_seek_by(int32_t delta_ms) {
  if (s_mu == NULL)
    return;
  lock();
  if (s_has_track) {
    int64_t target = (int64_t)s_pos_ms + delta_ms;
    if (target < 0)
      target = 0;
    if (s_dur_ms > 0 && target >= (int64_t)s_dur_ms) {
      next_locked(); // fast-forwarding off the end moves to the next song
    } else {
      s_cmd_seek = true;
      s_seek_ms = (uint32_t)target;
      s_pos_ms = (uint32_t)target;
    }
  }
  unlock();
  wake();
}

void ipod_player_set_shuffle(ipod_shuffle_t mode) {
  if (s_mu == NULL)
    return;
  lock();
  ipod_queue_set_shuffle(&s_queue, mode);
  unlock();
}

void ipod_player_set_repeat(ipod_repeat_t mode) {
  if (s_mu == NULL)
    return;
  lock();
  s_queue.repeat = mode < IPOD_REPEAT_COUNT ? mode : IPOD_REPEAT_OFF;
  unlock();
}

void ipod_player_set_eq(int preset) {
  if (s_mu == NULL)
    return;
  lock();
  s_eq = preset;
  s_dsp_dirty = true;
  unlock();
}

void ipod_player_set_sound_check(bool enabled) {
  if (s_mu == NULL)
    return;
  lock();
  s_sound_check = enabled;
  s_dsp_dirty = true;
  unlock();
}

void ipod_player_set_volume(uint8_t pct) {
  audio_i2s_set_volume(pct);
}

void ipod_player_click(void) {
  bool streaming = false;
  if (s_mu != NULL) {
    lock();
    streaming = s_stream_rate != 0;
    unlock();
  }
  if (streaming)
    atomic_store(&s_click, true); // the driver mutes UI sounds while streaming: mix it in
  else
    audio_click();
}

void ipod_player_get_status(ipod_player_status_t *out) {
  memset(out, 0, sizeof(*out));
  if (s_mu == NULL)
    return;
  lock();
  out->state = s_state;
  out->has_track = s_has_track;
  out->track = s_cur_track;
  out->pos_ms = s_pos_ms;
  out->dur_ms = s_dur_ms;
  out->queue_pos = s_queue.pos;
  out->queue_count = s_queue.count;
  out->track_seq = s_track_seq;
  out->end_seq = s_end_seq;
  out->error_seq = s_error_seq;
  out->out_rate = s_stream_rate;
  out->shuffle = s_queue.shuffle;
  out->repeat = s_queue.repeat;
  unlock();
}

size_t ipod_player_copy_queue(uint32_t **out) {
  *out = NULL;
  if (s_mu == NULL)
    return 0;
  lock();
  size_t n = s_queue.count;
  uint32_t *arr = n > 0 ? malloc(n * sizeof(uint32_t)) : NULL;
  if (arr != NULL) {
    for (size_t i = 0; i < n; i++)
      arr[i] = s_queue.tracks[s_queue.order[i]];
  } else {
    n = 0;
  }
  unlock();
  *out = arr;
  return n;
}

bool ipod_player_take_stats_dirty(void) {
  if (s_mu == NULL)
    return false;
  lock();
  bool dirty = s_stats_dirty;
  s_stats_dirty = false;
  unlock();
  return dirty;
}

// ---------------------------------------------------------------------------
// Decoder task
// ---------------------------------------------------------------------------

typedef struct {
  drflac *fl;
  ipod_dsp_t dsp;
  uint64_t frame_pos;
  uint32_t track;
  int16_t *pcm;
  int16_t *out;
  uint32_t paused_ms;
  size_t open_failures;
} decoder_t;

static void stream_close(void) {
  bool was_open;
  lock();
  was_open = s_stream_rate != 0;
  s_stream_rate = 0;
  unlock();
  if (was_open)
    audio_i2s_stream_stop();
}

static bool stream_ensure(uint32_t rate) {
  lock();
  uint32_t cur = s_stream_rate;
  unlock();
  if (cur == rate)
    return true; // same rate: keep streaming, which makes track changes gapless
  if (cur != 0)
    audio_i2s_stream_stop();
  if (audio_i2s_stream_start(rate) != ESP_OK) {
    ESP_LOGE(TAG, "stream start at %u Hz failed", (unsigned)rate);
    lock();
    s_stream_rate = 0;
    unlock();
    return false;
  }
  lock();
  s_stream_rate = rate;
  unlock();
  return true;
}

static void decoder_close(decoder_t *d) {
  if (d->fl != NULL) {
    drflac_close(d->fl);
    d->fl = NULL;
  }
}

static float track_gain_db(uint32_t track, bool sound_check) {
  if (!sound_check)
    return 0.0f;
  const ipod_track_t *t = ipod_lib_track(s_lib, track);
  if (t == NULL || t->rg_track_cdb == IPOD_RG_NONE)
    return 0.0f;
  return (float)t->rg_track_cdb / CDB_PER_DB;
}

// Open the queue's current song; on failure skip ahead like the iPod does.
static void decoder_open_current(decoder_t *d) {
  char path[PATH_LEN];
  uint32_t track = 0;
  bool have;
  int eq;
  bool sc;
  float gain = 0.0f;
  uint32_t lib_dur = 0;
  size_t count;

  decoder_close(d);
  lock();
  have = ipod_queue_current(&s_queue, &track);
  count = s_queue.count;
  if (have) {
    const ipod_track_t *t = ipod_lib_track(s_lib, track);
    have = t != NULL;
    if (have) {
      strncpy(path, ipod_lib_str(s_lib, t->path), sizeof(path) - 1);
      lib_dur = t->duration_ms;
    }
    path[sizeof(path) - 1] = '\0';
  }
  eq = s_eq;
  sc = s_sound_check;
  if (have)
    gain = track_gain_db(track, sc);
  unlock();
  if (!have)
    return;

  d->fl = drflac_open_file(path, NULL);
  if (d->fl != NULL &&
      (d->fl->channels == 0 || d->fl->channels > MAX_CHANNELS || d->fl->sampleRate == 0)) {
    decoder_close(d);
  }
  if (d->fl == NULL) {
    ESP_LOGW(TAG, "cannot play %s", path);
    lock();
    s_error_seq++;
    d->open_failures++;
    if (d->open_failures >= count) {
      stop_locked(); // nothing in the list is playable
      s_end_seq++;
    } else {
      next_locked();
    }
    unlock();
    return;
  }

  d->open_failures = 0;
  d->track = track;
  d->frame_pos = 0;
  ipod_dsp_configure(&d->dsp, d->fl->sampleRate, eq, gain);
  uint32_t dur = 0;
  if (d->fl->totalPCMFrameCount > 0)
    dur = (uint32_t)(d->fl->totalPCMFrameCount * MS_PER_SEC / d->fl->sampleRate);
  else
    dur = lib_dur;
  lock();
  s_has_track = true;
  s_cur_track = track;
  s_dur_ms = dur;
  s_pos_ms = 0;
  s_track_seq++;
  unlock();
  ESP_LOGI(TAG,
           "playing %s (%u Hz, %u ch, %u bit -> %u Hz mono)",
           path,
           (unsigned)d->fl->sampleRate,
           (unsigned)d->fl->channels,
           (unsigned)d->fl->bitsPerSample,
           (unsigned)d->dsp.out_rate);
}

static void decoder_seek(decoder_t *d, uint32_t ms) {
  if (d->fl == NULL)
    return;
  uint64_t frame = (uint64_t)ms * d->fl->sampleRate / MS_PER_SEC;
  if (d->fl->totalPCMFrameCount > 0 && frame >= d->fl->totalPCMFrameCount)
    frame = d->fl->totalPCMFrameCount - 1;
  if (drflac_seek_to_pcm_frame(d->fl, frame))
    d->frame_pos = frame;
  ipod_dsp_reset(&d->dsp);
}

// Called at the end of a song: count the play and move along the list.
static void decoder_track_finished(decoder_t *d) {
  decoder_close(d);
  lock();
  ipod_track_t *t = ipod_lib_track_mut(s_lib, d->track);
  if (t != NULL) {
    if (t->play_count < UINT16_MAX)
      t->play_count++;
    t->last_played = ++s_play_seq;
    s_stats_dirty = true;
  }
  if (ipod_queue_next(&s_queue, false)) {
    s_cmd_open = true;
  } else {
    stop_locked();
    s_end_seq++;
  }
  unlock();
}

static void player_task(void *arg) {
  (void)arg;
  decoder_t d = {0};
  d.pcm = malloc((size_t)CHUNK_OUT_SAMPLES * MAX_DECIM * MAX_CHANNELS * sizeof(int16_t));
  d.out = malloc((size_t)CHUNK_OUT_SAMPLES * sizeof(int16_t));
  if (d.pcm == NULL || d.out == NULL) {
    ESP_LOGE(TAG, "decode buffers alloc failed");
    goto cleanup;
  }

  for (;;) {
    lock();
    const bool quit = s_quit;
    const bool do_stop = s_cmd_stop;
    const bool do_open = s_cmd_open;
    const bool do_seek = s_cmd_seek;
    const uint32_t seek_ms = s_seek_ms;
    const bool dsp_dirty = s_dsp_dirty;
    const int eq = s_eq;
    const bool sc = s_sound_check;
    const ipod_play_state_t state = s_state;
    s_cmd_stop = false;
    s_cmd_open = false;
    s_cmd_seek = false;
    s_dsp_dirty = false;
    unlock();
    if (quit)
      break;

    if (do_stop) {
      decoder_close(&d);
      stream_close();
    }
    if (do_open)
      decoder_open_current(&d);
    if (do_seek)
      decoder_seek(&d, seek_ms);
    if (dsp_dirty && d.fl != NULL) {
      lock();
      float gain = track_gain_db(d.track, sc);
      unlock();
      ipod_dsp_configure(&d.dsp, d.fl->sampleRate, eq, gain);
    }

    if (d.fl == NULL || state == IPOD_PLAYER_STOPPED) {
      if (state == IPOD_PLAYER_STOPPED)
        decoder_close(&d);
      stream_close();
      xSemaphoreTake(s_wake, pdMS_TO_TICKS(IDLE_WAIT_MS));
      continue;
    }

    if (state == IPOD_PLAYER_PAUSED) {
      lock();
      const bool streaming = s_stream_rate != 0;
      unlock();
      if (streaming && d.paused_ms < PAUSE_SILENCE_MS) {
        // Silence keeps the I2S clock running so resuming does not pop.
        memset(d.out, 0, CHUNK_OUT_SAMPLES * sizeof(int16_t));
        audio_i2s_stream_write(d.out, CHUNK_OUT_SAMPLES);
        d.paused_ms += CHUNK_OUT_SAMPLES * MS_PER_SEC / d.dsp.out_rate;
      } else {
        stream_close();
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(IDLE_WAIT_MS));
      }
      continue;
    }

    d.paused_ms = 0;
    if (!stream_ensure(d.dsp.out_rate)) {
      lock();
      stop_locked();
      s_error_seq++;
      unlock();
      continue;
    }

    const size_t want = (size_t)CHUNK_OUT_SAMPLES * d.dsp.decim;
    const size_t got = (size_t)drflac_read_pcm_frames_s16(d.fl, want, d.pcm);
    if (atomic_exchange(&s_click, false))
      ipod_dsp_click(&d.dsp);
    const size_t n = ipod_dsp_process(&d.dsp, d.pcm, got, d.fl->channels, d.out);
    if (n > 0)
      audio_i2s_stream_write(d.out, (int)n);
    d.frame_pos += got;

    lock();
    if (!s_cmd_seek) // a pending seek already shows its target
      s_pos_ms = (uint32_t)(d.frame_pos * MS_PER_SEC / d.fl->sampleRate);
    unlock();

    if (got < want)
      decoder_track_finished(&d);
  }

cleanup:
  decoder_close(&d);
  stream_close();
  free(d.pcm);
  free(d.out);
  xSemaphoreGive(s_exited);
  vTaskDelete(NULL);
}
