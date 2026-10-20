// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// MP3 decode task - reads from the stream ring buffer, writes PCM to the codec and the
// spectrum analyzer.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the decode task (stopping a running one first). After audio_init() and stream_start().
void mp3dec_start(void);

// Stop the decode task and wait for it to exit.
void mp3dec_stop(void);

// True while the decode task is running.
bool mp3dec_is_running(void);

// True once audio has been decoded since the last start (the station is actually playing).
bool mp3dec_has_audio(void);

#ifdef __cplusplus
}
#endif
