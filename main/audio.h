// SPDX-License-Identifier: Apache-2.0
//
// Audio output, implemented by the board file: 16-bit stereo PCM to the board's codec.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Codec and I2S at 44100 Hz, 16-bit stereo. After board_init_power().
esp_err_t audio_init(void);

// Interleaved 16-bit stereo PCM (num_samples counts both channels)
esp_err_t audio_write(const int16_t *samples, size_t num_samples, size_t *samples_written,
                      uint32_t timeout_ms);

// The stream's sample rate (e.g. 22050, 44100, 48000)
esp_err_t audio_set_sample_rate(int rate);

// Volume 0-100
esp_err_t audio_set_volume(int vol);

// false mutes the output (the speaker and, where there is one, the headphone jack)
void audio_pa_enable(bool on);

#ifdef __cplusplus
}
#endif
