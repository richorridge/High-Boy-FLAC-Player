// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_DSP_H
#define IPOD_DSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_DSP_MAX_OUT_RATE 48000u
#define IPOD_EQ_BANDS         5
#define IPOD_EQ_OFF           0

typedef struct {
  float b0, b1, b2, a1, a2;
  float z1, z2;
} ipod_biquad_t;

/**
 * @brief Per-track signal chain feeding the High Boy's mono speaker:
 *        downmix -> decimate (hi-res) -> gain (Sound Check) -> EQ -> clicker.
 */
typedef struct {
  uint32_t in_rate;
  uint32_t out_rate;
  uint32_t decim; ///< 1, 2 or 4: hi-res input is averaged down to <= 48 kHz
  int eq_preset;
  ipod_biquad_t bands[IPOD_EQ_BANDS];
  int nbands;
  float gain; ///< linear, Sound Check x EQ pre-gain
  bool use_float;
  uint32_t click_len;
  uint32_t click_pos; ///< samples left in the current click
} ipod_dsp_t;

/** @brief Number of EQ presets (index 0 is "Off"). */
int ipod_eq_count(void);
/** @brief iPod preset name, e.g. "Bass Booster". */
const char *ipod_eq_name(int preset);

/**
 * @brief (Re)configure the chain for a track.
 * @param gain_db  Sound Check gain (0 for none).
 */
void ipod_dsp_configure(ipod_dsp_t *dsp, uint32_t in_rate, int eq_preset, float gain_db);

/** @brief Clear filter history (after a seek or track change). */
void ipod_dsp_reset(ipod_dsp_t *dsp);

/** @brief Queue one clicker tick to be mixed into the next output. */
void ipod_dsp_click(ipod_dsp_t *dsp);

/**
 * @brief Process interleaved 16-bit frames into mono output.
 * @param frames  number of input frames; must be a multiple of dsp->decim
 * @param out     room for frames / decim samples
 * @return number of mono samples written
 */
size_t ipod_dsp_process(ipod_dsp_t *dsp,
                        const int16_t *in,
                        size_t frames,
                        unsigned channels,
                        int16_t *out);

#ifdef __cplusplus
}
#endif

#endif // IPOD_DSP_H
