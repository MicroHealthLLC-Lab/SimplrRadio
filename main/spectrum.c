// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Spectrum analyzer using esp-dsp FFT.
// Receives PCM from the MP3 decoder, computes 32 log-spaced frequency bands
// with exponential decay and peak hold for smooth visualization.

#include <string.h>
#include <math.h>
#include "spectrum.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dsps_fft2r.h"
#include "dsps_wind_hann.h"

#define TAG "spectrum"

// FFT input: interleaved Re/Im pairs (Im=0 for real input)
static float s_fft_buf[SPECTRUM_FFT_N * 2];
static float s_window[SPECTRUM_FFT_N];

// PCM accumulation buffer (mono, downmixed from stereo)
static float s_pcm_buf[SPECTRUM_FFT_N];
static int   s_pcm_pos = 0;

// Output bands with decay
static float s_bands[SPECTRUM_BANDS];
static float s_peaks[SPECTRUM_BANDS];
static SemaphoreHandle_t s_mutex;
static TickType_t s_last_fft;      // When bands were last computed

// Log-spaced band edges for 32 bands spanning FFT bins 1..N/2
// Each band covers an exponentially wider range of bins.
static int s_bin_lo[SPECTRUM_BANDS];
static int s_bin_hi[SPECTRUM_BANDS];

static void compute_band_edges(void)
{
    // Map 32 bands logarithmically across bins 1..(N/2-1)
    const int max_bin = SPECTRUM_FFT_N / 2;
    const float log_min = logf(1.0f);
    const float log_max = logf((float)max_bin);

    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        float lo = expf(log_min + (log_max - log_min) * i / SPECTRUM_BANDS);
        float hi = expf(log_min + (log_max - log_min) * (i + 1) / SPECTRUM_BANDS);
        s_bin_lo[i] = (int)lo;
        s_bin_hi[i] = (int)hi;
        if (s_bin_hi[i] <= s_bin_lo[i]) s_bin_hi[i] = s_bin_lo[i] + 1;
        if (s_bin_hi[i] > max_bin) s_bin_hi[i] = max_bin;
    }
}

void spectrum_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    dsps_fft2r_init_fc32(NULL, SPECTRUM_FFT_N);
    dsps_wind_hann_f32(s_window, SPECTRUM_FFT_N);
    compute_band_edges();   // s_bands and s_peaks are static: they start at zero
    ESP_LOGI(TAG, "Spectrum init: %d-point FFT, %d bands", SPECTRUM_FFT_N, SPECTRUM_BANDS);
}

// Run FFT on the accumulated PCM buffer and update bands
static void run_fft(void)
{
    // Apply window and pack into complex array (Im = 0)
    for (int i = 0; i < SPECTRUM_FFT_N; i++) {
        s_fft_buf[i * 2 + 0] = s_pcm_buf[i] * s_window[i];
        s_fft_buf[i * 2 + 1] = 0.0f;
    }

    dsps_fft2r_fc32(s_fft_buf, SPECTRUM_FFT_N);
    // Bit-reverse (required after radix-2 FFT)
    dsps_bit_rev_fc32_ansi(s_fft_buf, SPECTRUM_FFT_N);

    // Compute magnitude for each band
    float new_bands[SPECTRUM_BANDS];
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        float sum = 0;
        for (int k = s_bin_lo[b]; k < s_bin_hi[b]; k++) {
            float re = s_fft_buf[k * 2 + 0];
            float im = s_fft_buf[k * 2 + 1];
            sum += sqrtf(re * re + im * im);
        }
        int n_bins = s_bin_hi[b] - s_bin_lo[b];
        if (n_bins > 0) sum /= n_bins;

        // Normalize: divide by FFT_N/2 and apply log scale
        sum /= (SPECTRUM_FFT_N / 2);
        // Convert to dB-ish scale (0..1 range)
        // 20*log10(sum) mapped to roughly 0..1 for typical audio levels
        if (sum > 1e-6f) {
            sum = 1.0f + (log10f(sum) / 3.5f);  // ~70dB range -> 0..1
            if (sum < 0) sum = 0;
            if (sum > 1.0f) sum = 1.0f;
        } else {
            sum = 0;
        }
        new_bands[b] = sum;
    }

    // Update bands with decay
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        // Smooth rise, exponential decay
        if (new_bands[b] > s_bands[b]) {
            s_bands[b] = new_bands[b];
        } else {
            s_bands[b] *= 0.85f;
        }
        // Peak hold with slow fall
        if (new_bands[b] > s_peaks[b]) {
            s_peaks[b] = new_bands[b];
        } else {
            s_peaks[b] *= 0.97f;
        }
    }
    s_last_fft = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
}

void spectrum_feed(const int16_t *samples, int num_stereo_samples)
{
    // Downmix stereo to mono float, accumulate into PCM buffer
    for (int i = 0; i < num_stereo_samples; i += 2) {
        float mono = ((float)samples[i] + (float)samples[i + 1]) / 65536.0f;
        s_pcm_buf[s_pcm_pos++] = mono;

        if (s_pcm_pos >= SPECTRUM_FFT_N) {
            run_fft();
            s_pcm_pos = 0;
        }
    }
}

void spectrum_snapshot(float *bands, float *peaks)
{
    if (!s_mutex) {
        for (int b = 0; b < SPECTRUM_BANDS; b++) {
            bands[b] = peaks[b] = 0.0f;
        }
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // No audio for a moment (muted, paused, station changing): let the bars fall
    if (xTaskGetTickCount() - s_last_fft > pdMS_TO_TICKS(150)) {
        for (int b = 0; b < SPECTRUM_BANDS; b++) {
            s_bands[b] *= 0.80f;
            s_peaks[b] *= 0.92f;
        }
    }
    // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - the caller's arrays are float[SPECTRUM_BANDS] (spectrum.h), like s_bands
    memcpy(bands, s_bands, sizeof(s_bands));
    // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - the caller's arrays are float[SPECTRUM_BANDS] (spectrum.h), like s_peaks
    memcpy(peaks, s_peaks, sizeof(s_peaks));
    xSemaphoreGive(s_mutex);
}
