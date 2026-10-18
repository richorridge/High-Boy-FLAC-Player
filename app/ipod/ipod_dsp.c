// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_dsp.h"

#include <math.h>
#include <string.h>

#define PI_F             3.14159265f
#define PEAK_Q           0.9f
#define SHELF_SLOPE      1.0f
#define BAND_MAX_FRACT   0.45f // skip bands too close to Nyquist
#define PREGAIN_FRACTION 0.6f  // headroom for boosted presets
#define GAIN_MIN_DB      -15.0f
#define GAIN_MAX_DB      10.0f
#define DB_PER_DECADE    20.0f
#define DB_EPSILON       0.01f
#define CLICK_MS         2u
#define CLICK_AMPLITUDE  6000
#define MS_PER_SEC       1000u
#define INT16_MAX_F      32767.0f
#define INT16_MIN_F      -32768.0f

typedef enum {
  BAND_LOW_SHELF = 0,
  BAND_PEAK,
  BAND_HIGH_SHELF,
} band_kind_t;

typedef struct {
  float hz;
  band_kind_t kind;
} band_def_t;

static const band_def_t BANDS[IPOD_EQ_BANDS] = {
    {100.0f, BAND_LOW_SHELF},
    {230.0f, BAND_PEAK},
    {910.0f, BAND_PEAK},
    {3600.0f, BAND_PEAK},
    {6000.0f, BAND_HIGH_SHELF},
};

typedef struct {
  const char *name;
  float db[IPOD_EQ_BANDS];
} eq_preset_t;

// The iPod's preset list, folded from iTunes' 10-band curves onto 5 bands.
static const eq_preset_t PRESETS[] = {
    {"Off", {0, 0, 0, 0, 0}},
    {"Acoustic", {5.0f, 2.5f, 1.5f, 3.75f, 2.5f}},
    {"Bass Booster", {5.0f, 3.0f, 0.75f, 0, 0}},
    {"Bass Reducer", {-5.0f, -3.0f, -0.75f, 0, 0}},
    {"Classical", {4.0f, 2.75f, -1.5f, 1.0f, 3.25f}},
    {"Dance", {5.0f, 2.5f, 2.75f, 4.75f, 1.75f}},
    {"Deep", {4.25f, 2.0f, 2.75f, 0, -4.0f}},
    {"Electronic", {4.25f, 0.75f, 0, 1.25f, 4.5f}},
    {"Flat", {0, 0, 0, 0, 0}},
    {"Hip Hop", {4.5f, 2.25f, -1.0f, 0.5f, 2.5f}},
    {"Jazz", {3.5f, 1.75f, -1.5f, 0.75f, 3.25f}},
    {"Latin", {3.75f, 0, -1.5f, -0.75f, 3.75f}},
    {"Loudness", {5.0f, 0, -1.0f, -3.0f, 3.0f}},
    {"Lounge", {-2.25f, 0.5f, 3.25f, -0.75f, 1.5f}},
    {"Piano", {2.5f, 1.25f, 2.25f, 4.0f, 3.25f}},
    {"Pop", {-1.25f, 1.0f, 4.0f, 1.0f, -1.25f}},
    {"R&B", {4.75f, 3.5f, -1.75f, 2.25f, 3.25f}},
    {"Rock", {4.5f, 2.25f, -0.75f, 1.5f, 4.0f}},
    {"Small Speakers", {4.75f, 3.0f, 0.5f, -1.75f, -3.5f}},
    {"Spoken Word", {-2.0f, 0.25f, 4.25f, 4.75f, 1.25f}},
    {"Treble Booster", {0, 0, 0.5f, 3.0f, 5.0f}},
    {"Treble Reducer", {0, 0, -0.5f, -3.0f, -5.0f}},
    {"Vocal Booster", {-2.25f, -0.75f, 3.5f, 2.25f, -0.75f}},
};

#define PRESET_COUNT ((int)(sizeof(PRESETS) / sizeof(PRESETS[0])))

static void design_band(ipod_biquad_t *bq, band_kind_t kind, float hz, float db, float fs);

int ipod_eq_count(void) {
  return PRESET_COUNT;
}

const char *ipod_eq_name(int preset) {
  if (preset < 0 || preset >= PRESET_COUNT)
    return PRESETS[IPOD_EQ_OFF].name;
  return PRESETS[preset].name;
}

void ipod_dsp_configure(ipod_dsp_t *dsp, uint32_t in_rate, int eq_preset, float gain_db) {
  memset(dsp, 0, sizeof(*dsp));
  dsp->in_rate = in_rate;
  dsp->decim = 1;
  while (in_rate / dsp->decim > IPOD_DSP_MAX_OUT_RATE && dsp->decim < 4)
    dsp->decim *= 2;
  dsp->out_rate = in_rate / dsp->decim;
  dsp->eq_preset = (eq_preset >= 0 && eq_preset < PRESET_COUNT) ? eq_preset : IPOD_EQ_OFF;
  dsp->click_len = dsp->out_rate * CLICK_MS / MS_PER_SEC;

  if (gain_db < GAIN_MIN_DB)
    gain_db = GAIN_MIN_DB;
  if (gain_db > GAIN_MAX_DB)
    gain_db = GAIN_MAX_DB;

  float max_boost = 0.0f;
  const eq_preset_t *p = &PRESETS[dsp->eq_preset];
  for (int b = 0; b < IPOD_EQ_BANDS; b++) {
    float db = p->db[b];
    if (fabsf(db) < DB_EPSILON || BANDS[b].hz >= BAND_MAX_FRACT * (float)dsp->out_rate)
      continue;
    design_band(&dsp->bands[dsp->nbands++], BANDS[b].kind, BANDS[b].hz, db, (float)dsp->out_rate);
    if (db > max_boost)
      max_boost = db;
  }
  float total_db = gain_db - max_boost * PREGAIN_FRACTION;
  dsp->gain = powf(10.0f, total_db / DB_PER_DECADE);
  dsp->use_float = dsp->nbands > 0 || fabsf(total_db) >= DB_EPSILON;
}

void ipod_dsp_reset(ipod_dsp_t *dsp) {
  for (int b = 0; b < dsp->nbands; b++) {
    dsp->bands[b].z1 = 0.0f;
    dsp->bands[b].z2 = 0.0f;
  }
}

void ipod_dsp_click(ipod_dsp_t *dsp) {
  dsp->click_pos = dsp->click_len;
}

size_t ipod_dsp_process(
    ipod_dsp_t *dsp, const int16_t *in, size_t frames, unsigned channels, int16_t *out) {
  if (channels == 0 || dsp->decim == 0)
    return 0;
  const size_t n_out = frames / dsp->decim;
  const int32_t div = (int32_t)(dsp->decim * channels);
  for (size_t o = 0; o < n_out; o++) {
    const int16_t *src = in + o * dsp->decim * channels;
    int32_t acc = 0;
    for (int32_t k = 0; k < div; k++)
      acc += src[k];
    int32_t mono = acc / div;

    if (dsp->use_float) {
      float x = (float)mono * dsp->gain;
      for (int b = 0; b < dsp->nbands; b++) {
        ipod_biquad_t *bq = &dsp->bands[b];
        float y = bq->b0 * x + bq->z1; // transposed direct form II
        bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
        bq->z2 = bq->b2 * x - bq->a2 * y;
        x = y;
      }
      if (x > INT16_MAX_F)
        x = INT16_MAX_F;
      else if (x < INT16_MIN_F)
        x = INT16_MIN_F;
      mono = (int32_t)x;
    }

    if (dsp->click_pos > 0) {
      // A short decaying blip, like the iPod's piezo clicker.
      uint32_t k = dsp->click_len - dsp->click_pos;
      int32_t amp = CLICK_AMPLITUDE * (int32_t)dsp->click_pos / (int32_t)dsp->click_len;
      mono += (k < dsp->click_len / 2) ? amp : -amp;
      dsp->click_pos--;
      if (mono > INT16_MAX)
        mono = INT16_MAX;
      else if (mono < INT16_MIN)
        mono = INT16_MIN;
    }
    out[o] = (int16_t)mono;
  }
  return n_out;
}

// RBJ Audio EQ Cookbook filters, normalised so a0 == 1.
static void design_band(ipod_biquad_t *bq, band_kind_t kind, float hz, float db, float fs) {
  const float a = powf(10.0f, db / (2.0f * DB_PER_DECADE));
  const float w0 = 2.0f * PI_F * hz / fs;
  const float cw = cosf(w0);
  const float sw = sinf(w0);
  float b0, b1, b2, a0, a1, a2;
  if (kind == BAND_PEAK) {
    const float alpha = sw / (2.0f * PEAK_Q);
    b0 = 1.0f + alpha * a;
    b1 = -2.0f * cw;
    b2 = 1.0f - alpha * a;
    a0 = 1.0f + alpha / a;
    a1 = -2.0f * cw;
    a2 = 1.0f - alpha / a;
  } else {
    const float alpha = sw / 2.0f * sqrtf((a + 1.0f / a) * (1.0f / SHELF_SLOPE - 1.0f) + 2.0f);
    const float sq = 2.0f * sqrtf(a) * alpha;
    if (kind == BAND_LOW_SHELF) {
      b0 = a * ((a + 1.0f) - (a - 1.0f) * cw + sq);
      b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cw);
      b2 = a * ((a + 1.0f) - (a - 1.0f) * cw - sq);
      a0 = (a + 1.0f) + (a - 1.0f) * cw + sq;
      a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cw);
      a2 = (a + 1.0f) + (a - 1.0f) * cw - sq;
    } else {
      b0 = a * ((a + 1.0f) + (a - 1.0f) * cw + sq);
      b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cw);
      b2 = a * ((a + 1.0f) + (a - 1.0f) * cw - sq);
      a0 = (a + 1.0f) - (a - 1.0f) * cw + sq;
      a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cw);
      a2 = (a + 1.0f) - (a - 1.0f) * cw - sq;
    }
  }
  bq->b0 = b0 / a0;
  bq->b1 = b1 / a0;
  bq->b2 = b2 / a0;
  bq->a1 = a1 / a0;
  bq->a2 = a2 / a0;
  bq->z1 = 0.0f;
  bq->z2 = 0.0f;
}
