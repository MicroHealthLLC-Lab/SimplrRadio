// SPDX-License-Identifier: Apache-2.0
//
// Genres from the internet for songs whose files have none: the MusicBrainz database
// (musicbrainz.org, open data, no account or key), asked per artist. HTTPS verified with the CA
// bundle; the artist's name is the only thing sent. MusicBrainz asks for at most one request a
// second from a device: each lookup is two requests, and music_player.cpp spaces lookups out.

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_ONLINE_FOUND,     // genre (and name) filled in
    MUSIC_ONLINE_NONE,      // The artist isn't known well enough, or has no genre there
    MUSIC_ONLINE_BUSY,      // MusicBrainz asked to slow down (HTTP 503): try later
    MUSIC_ONLINE_ERROR,     // No connection, or a reply that couldn't be read
} music_online_result_t;

// Blocking (worker task). genre: the artist's most voted genre ("Electronic"); name: the artist
// as MusicBrainz spells it, when it differs only in capitals from `artist`, else "".
music_online_result_t music_online_artist(const char *artist, char *genre, size_t genre_len, char *name,
                                          size_t name_len);

#ifdef __cplusplus
}
#endif
