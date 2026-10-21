// SPDX-License-Identifier: Apache-2.0
//
// The music player: songs from the SD card as the player's source, next to the radio. The C
// part is for player.c, which hands every Now Playing control (next / previous, pause / play,
// status) to it while music is the source; the C++ part (music namespace) is the app's: the
// library on the card, the play queue, shuffle / repeat, favorites, the online genres.
// Called from the app task; the functions marked "worker" block (SD card, network).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool music_active(void);              // A song is the source (playing, paused or ended)
void music_leave(void);               // The radio takes over: the decoder stops (waits)
void music_step(int delta);           // Next (+1) / previous (-1) song, debounced
void music_pause(void);
void music_play(void);
bool music_paused(void);
void music_tick(void);                // Commits a pending song, follows the decoder (waits)
int music_status(void);               // A player_status_t
uint32_t music_version(void);         // Changes with anything Now Playing shows

#ifdef __cplusplus
}

#include "music_library.hpp"
#include <string>
#include <vector>

namespace music
{
    // The library and its state (app task)
    Library& library();
    GenreCache& genres();
    Settings& settings();                    // Change, then save_settings(settings())
    std::vector<uint32_t>& favorites();
    bool loaded();                           // The library was read (from the index or a scan)
    std::string folder_shown();              // The music folder for the screen ("/Music", "the whole card")

    // Worker: the card, the index files, the folder walk
    esp_err_t mount();
    bool load_index(const std::string& folder, Library& into);
    struct ScanResult {
        int songs = 0;
        int too_long = 0;                    // Paths over PATH_LEN, or folders too deep
        int unreadable = 0;                  // Files that aren't what their name says
        bool full = false;                   // MAX_TRACKS reached
        bool card = false;                   // The folder could be opened
    };
    ScanResult scan(const std::string& folder, Library& into, const GenreCache& cache);
    int scan_progress();                     // Songs found so far by a running scan
    bool save_index(const std::string& folder, const Library& lib);
    bool load_genres(GenreCache& into);
    bool save_genres(const GenreCache& cache);
    // The folders in `folder` (relative to the card, "/" = its top), sorted, at most MAX_FOLDERS
    static const int MAX_FOLDERS = 200;
    std::vector<std::string> list_folders(const std::string& folder, bool& ok);

    // App task: a new library (from load_index / scan) takes over; the queue follows by path
    void install(Library& built);

    // Playing. play() stops a song that is playing: call it past the display lock (defer).
    void play(const Indexes& songs, int start);
    int playing();                           // The song (library index), -1 if none
    const Queue& queue();
    uint32_t elapsed_ms();
    void set_shuffle(bool on);
    void set_repeat(Repeat r);
    bool toggle_favorite(int song);          // false: the favorites list is full
    bool is_favorite(int song);
}
#endif
