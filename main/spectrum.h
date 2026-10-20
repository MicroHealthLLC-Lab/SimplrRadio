// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Spectrum analyzer - FFT on PCM samples, outputs 32 log-scale bands

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPECTRUM_BANDS  32
#define SPECTRUM_FFT_N  512

// Initialize FFT tables and window. Call once at startup.
void spectrum_init(void);

// Feed interleaved stereo 16-bit PCM samples (from the decode task only).
// Accumulates into an internal buffer; runs FFT when full.
void spectrum_feed(const int16_t *samples, int num_stereo_samples);

// Copies the band magnitudes and their peaks (0.0-1.0, SPECTRUM_BANDS floats each). Thread-safe;
// bands decay toward zero while no audio is being fed.
void spectrum_snapshot(float *bands, float *peaks);

#ifdef __cplusplus
}
#endif
