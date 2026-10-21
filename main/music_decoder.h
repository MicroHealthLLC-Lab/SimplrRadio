// SPDX-License-Identifier: Apache-2.0
//
// Music file decoder: one file from the SD card at a time, decoded by Espressif's
// esp_audio_codec (its "simple decoder" parses the container: MP3, AAC, M4A with AAC or ALAC,
// FLAC, Ogg Vorbis / Opus, WAV, AMR) and written to the board's audio output and the spectrum
// as 16-bit stereo, whatever the file's bit depth and channels. A task ("filedec") per file.
// Called from the app task; music_decoder_start / _stop wait for the previous task to leave.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "media_tags.h"

#ifdef __cplusplus
extern "C" {
#endif

// Plays `path` (the full path on the card) from `audio_offset` (past a leading ID3 tag).
// Stops a file that is playing first.
void music_decoder_start(const char *path, media_format_t format, uint32_t audio_offset);
void music_decoder_stop(void);

// Paused: no more audio until resumed, the place in the file is kept
void music_decoder_pause(bool paused);

bool music_decoder_running(void);     // The task is decoding (or paused)
bool music_decoder_has_audio(void);   // Audio came out since the start
bool music_decoder_finished(void);    // The file played to its end
bool music_decoder_failed(void);      // The file couldn't be opened or decoded
uint32_t music_decoder_elapsed_ms(void);

#ifdef __cplusplus
}
#endif
