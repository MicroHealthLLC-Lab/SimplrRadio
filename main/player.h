// SPDX-License-Identifier: Apache-2.0
//
// Player: the station list, tuning (debounced, so paging through stations doesn't start and
// stop a stream for each one), stream health (a station that drops is retried, then skipped),
// volume and mute (saved to flash a moment after the last change, not on every step).
// Its source is the radio or music from the SD card (music_player.h): while music plays, next /
// previous, pause / play and the status are the music player's, a lost WiFi link doesn't stop
// it, and picking a station hands the output back to the radio.
// Called from the app task; player_tick() must run outside the display lock (stopping a
// stream waits for its tasks).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "radio_browser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLAYER_IDLE,          // No station chosen (no list yet, or stopped)
    PLAYER_CONNECTING,    // Stream opening / buffering
    PLAYER_PLAYING,       // Audio is coming out
    PLAYER_UNAVAILABLE,   // The station refused the stream (e.g. HTTP 403, geo-blocked)
    PLAYER_PAUSED,        // The user paused: nothing streams until Play or a station is picked
} player_status_t;

esp_err_t player_init(void);            // Audio, spectrum, saved volume

// Replaces the station list (copied). Keeps playing the current station if it is still in
// the list; otherwise tunes the last station played (saved), else the first.
void player_set_stations(const radio_station_t *list, int count);
int player_count(void);
int player_index(void);                 // -1 without stations
const radio_station_t *player_station(int index);   // NULL if out of range

void player_select(int index);          // Tune (debounced); ends a pause and any music
void player_step(int delta);            // Next (+1) / previous (-1), wrapping
void player_stop(void);                 // Stop the stream (WiFi down)
void player_resume(void);               // Tune the current station again (WiFi back), unless paused
void player_pause(void);                // The user stops the radio: stream closed, stays stopped
void player_play(void);                 // Ends a pause: the current station again
bool player_paused(void);
void player_tick(void);                 // Commits a pending tune, watches the stream

// Music takes the audio output (music_player.cpp): the stream stops, the station stays chosen
void player_release(void);
// At boot, when music played last: a station list that arrives doesn't start the radio until
// player_set_hold(false) (then it does, unless music started meanwhile)
void player_set_hold(bool hold);

player_status_t player_status(void);
// Why the station is unavailable ("HTTP 403", "no connection"...), "" if unknown
const char *player_error(void);
void player_title(char *buf, size_t len);   // Current song (ICY), "" if none

int player_volume(void);
void player_set_volume(int volume);     // 0-100; unmutes
bool player_muted(void);
void player_set_muted(bool muted);

// Changes whenever anything shown on the Now Playing screen changes
uint32_t player_version(void);

#ifdef __cplusplus
}
#endif
