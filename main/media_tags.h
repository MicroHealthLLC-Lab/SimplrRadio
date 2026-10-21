// SPDX-License-Identifier: Apache-2.0
//
// Audio files for the music library (music_library.cpp): the format from the file name, the
// tags inside the file (ID3v1 / ID3v2.2-2.4, FLAC and Ogg Vorbis / Opus comments, MP4 / M4A
// items, WAV INFO), the duration, and the title / artist / album worked out from the file's
// path when the tags don't say. Files on the SD card are untrusted input: every size in them is
// checked against the file and against fixed bounds, and text reaches the tags only through
// text_to_ascii() (printable ASCII). No ESP-IDF headers (host tests).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_NONE = 0,
    MEDIA_MP3,      // .mp3 (MPEG audio layer 1/2/3)
    MEDIA_AAC,      // .aac (ADTS)
    MEDIA_M4A,      // .m4a .mp4 .m4b: AAC in MP4
    MEDIA_ALAC,     // Apple Lossless in MP4 (found inside an .m4a)
    MEDIA_FLAC,     // .flac
    MEDIA_VORBIS,   // .ogg .oga: Vorbis in Ogg
    MEDIA_OPUS,     // .opus (or an .ogg holding Opus)
    MEDIA_WAV,      // .wav (PCM, ADPCM, G.711)
    MEDIA_AMR,      // .amr (AMR-NB)
    MEDIA_AMRWB,    // .awb (AMR-WB)
    MEDIA_FORMATS
} media_format_t;

#define MEDIA_TITLE_LEN  64
#define MEDIA_NAME_LEN   48    // Artist, album
#define MEDIA_GENRE_LEN  32

#define MEDIA_TEXT_MAX      512           // Longest tag value read (bytes, before conversion)
#define MEDIA_COMMENTS_MAX  (4 * 1024)    // Vorbis comment block read (FLAC, Ogg)
#define MEDIA_OGG_TAIL      (24 * 1024)   // End of an Ogg file searched for its length

typedef struct {
    char title[MEDIA_TITLE_LEN];
    char artist[MEDIA_NAME_LEN];
    char album[MEDIA_NAME_LEN];
    char genre[MEDIA_GENRE_LEN];
    uint16_t year;
    uint16_t track;
    uint32_t duration_ms;     // 0 = unknown
    uint32_t audio_offset;    // Where the decoder starts (past a leading ID3v2 tag)
    media_format_t format;    // From the name, refined by the content (Opus, ALAC)
    bool playable;            // false: a variant the decoder can't play (see media_tags_read)
} media_tags_t;

// A file read through a callback (the SD card on the radio, memory in the host tests)
typedef struct {
    // Up to len bytes at offset into buf; the number read (0 past the end or on an error)
    size_t (*read)(void *ctx, uint64_t offset, uint8_t *buf, size_t len);
    void *ctx;
    uint64_t size;
} media_file_t;

// The format a file name stands for by its extension (case-insensitive), MEDIA_NONE if it isn't
// an audio file the radio plays
media_format_t media_format_from_name(const char *name);
const char *media_format_name(media_format_t format);   // "MP3", "FLAC", ...

// Reads the tags and length of `file`, which has `format` (from its name). Fields the file
// doesn't have stay empty / 0. playable is false for an M4A whose index (moov) comes after
// the audio and for floating-point WAV, which the decoder can't stream. Returns false when the
// file isn't that format at all (wrong extension, truncated, not audio).
bool media_tags_read(const media_file_t *file, media_format_t format, media_tags_t *out);

// Fills what the tags left empty from the file's path relative to the music folder:
//   "Artist/Album/03 - Title.mp3", "Artist - Album/Title.flac", "Artist - Title.ogg",
//   "CD1" / "Disc 2" folders skipped. The title falls back to the file name. Text that is all
// capitals or all lower case gets capitals at the start of each word ("daft punk" -> "Daft Punk").
void media_tags_from_path(const char *rel_path, media_tags_t *tags);

// A genre as taggers write it ("(17)", "17", "(17)Rock", "rock; pop", "Hip Hop/Rap", "RX") as
// one name ("Rock", "Hip-Hop", "Remix"); "" when there is none
void media_genre_clean(char *dst, size_t len, const char *src);

// The ID3v1 genre list (with Winamp's extensions, 0-191); NULL out of range
const char *media_id3_genre(int index);

// "Daft Punk" for "daft punk" / "DAFT PUNK"; mixed case is left alone
void media_fix_case(char *text);

#ifdef __cplusplus
}
#endif
