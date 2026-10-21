// SPDX-License-Identifier: Apache-2.0
//
// Audio file tags (media_tags.h). Formats as their specifications describe them:
//   ID3v2   id3.org/id3v2.3.0, id3v2.4.0-structure / -frames (and v2.2's three-letter frames)
//   ID3v1   the 128-byte "TAG" block at the end; genres with Winamp's extensions
//   FLAC    xiph.org/flac/format.html: STREAMINFO, VORBIS_COMMENT
//   Ogg     RFC 3533 pages; Vorbis I identification / comment headers; RFC 7845 (Opus)
//   MP4     ISO/IEC 14496-12 boxes; iTunes' moov/udta/meta/ilst items
//   WAV     RIFF "fmt ", "data", LIST/INFO and an embedded "id3 " chunk
// Every length read from a file is checked against the file's size and the bounds in
// media_tags.h before it is used; text goes through text_to_ascii().

#include "media_tags.h"
#include "text.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

static const struct {
    const char *ext;
    media_format_t format;
} EXTENSIONS[] = {
    {"mp3", MEDIA_MP3}, {"mp2", MEDIA_MP3}, {"aac", MEDIA_AAC}, {"m4a", MEDIA_M4A}, {"mp4", MEDIA_M4A},
    {"m4b", MEDIA_M4A}, {"flac", MEDIA_FLAC}, {"ogg", MEDIA_VORBIS}, {"oga", MEDIA_VORBIS},
    {"opus", MEDIA_OPUS}, {"wav", MEDIA_WAV}, {"amr", MEDIA_AMR}, {"awb", MEDIA_AMRWB},
};

static const char *const FORMAT_NAMES[MEDIA_FORMATS] = {
    "", "MP3", "AAC", "AAC", "ALAC", "FLAC", "Vorbis", "Opus", "WAV", "AMR", "AMR-WB",
};

media_format_t media_format_from_name(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || dot == name) {
        return MEDIA_NONE;   // No extension, or a hidden file (".mp3")
    }
    for (size_t i = 0; i < sizeof(EXTENSIONS) / sizeof(EXTENSIONS[0]); i++) {
        if (strcasecmp(dot + 1, EXTENSIONS[i].ext) == 0) {
            return EXTENSIONS[i].format;
        }
    }
    return MEDIA_NONE;
}

const char *media_format_name(media_format_t format)
{
    return format > MEDIA_NONE && format < MEDIA_FORMATS ? FORMAT_NAMES[format] : "";
}

// ---------------------------------------------------------------------------
// Genres
// ---------------------------------------------------------------------------

static const char *const ID3_GENRES[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
    "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka",
    "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk", "Folk-Rock", "National Folk", "Swing",
    "Fast Fusion", "Bebop", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde", "Gothic Rock",
    "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band", "Chorus",
    "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music", "Sonata",
    "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock", "Drum Solo",
    "A Cappella", "Euro-House", "Dance Hall", "Goa", "Drum & Bass", "Club-House", "Hardcore", "Terror",
    "Indie", "Britpop", "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap", "Heavy Metal",
    "Black Metal", "Crossover", "Contemporary Christian", "Christian Rock", "Merengue", "Salsa",
    "Thrash Metal", "Anime", "J-Pop", "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra",
    "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM", "Eclectic", "Electro",
    "Electroclash", "Emo", "Experimental", "Garage", "Global", "IDM", "Illbient", "Industro-Goth",
    "Jam Band", "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic", "Nu-Breakz",
    "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze", "Space Rock", "Trop Rock", "World Music",
    "Neoclassical", "Audiobook", "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock",
    "G-Funk", "Dubstep", "Garage Rock", "Psybient",
};
#define ID3_GENRE_COUNT ((int)(sizeof(ID3_GENRES) / sizeof(ID3_GENRES[0])))

const char *media_id3_genre(int index)
{
    return index >= 0 && index < ID3_GENRE_COUNT ? ID3_GENRES[index] : NULL;
}

// Spellings of the same genre that taggers mix
static const struct {
    const char *from;
    const char *to;
} GENRE_SAME[] = {
    {"hip hop", "Hip-Hop"}, {"hiphop", "Hip-Hop"}, {"rnb", "R&B"}, {"r and b", "R&B"},
    {"rhythm and blues", "R&B"}, {"electronica", "Electronic"}, {"alternrock", "Alternative Rock"},
    {"rock and roll", "Rock & Roll"}, {"rock n roll", "Rock & Roll"}, {"synth-pop", "Synthpop"},
    {"drum and bass", "Drum & Bass"}, {"dnb", "Drum & Bass"}, {"soundtracks", "Soundtrack"},
};

void media_fix_case(char *text)
{
    int upper = 0, lower = 0;
    bool words = false;
    for (const char *p = text; *p; p++) {
        upper += isupper((unsigned char)*p) != 0;
        lower += islower((unsigned char)*p) != 0;
        words = words || *p == ' ';
    }
    // Mixed case is meant ("iPod", "deadmau5 feat. Kaskade"), and so are capitals of one word or
    // a short name ("ABBA", "AC/DC", "R.E.M."): only all-lower text, or capitals over several
    // words ("LED ZEPPELIN"), gets fixed
    if ((upper && lower) || upper + lower < 2 || (upper && (!words || upper < 8))) {
        return;
    }
    bool start = true;
    for (char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalpha(c)) {
            *p = (char)(start ? toupper(c) : tolower(c));
            start = false;
        } else {
            start = c == ' ' || c == '-' || c == '(' || c == '/' || c == '.' || c == '"';
        }
    }
}

// A whole string of 1 to 3 digits (in a buffer of `cap` bytes) as a number, else -1
static int small_number(const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (n == 0 || n > 3 || strspn(s, "0123456789") != n) {
        return -1;
    }
    return (int)strtol(s, NULL, 10);
}

// Without leading spaces and trailing spaces / dots; s is a buffer of `cap` bytes
static void trim(char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '.')) {
        s[--n] = '\0';
    }
    size_t lead = strspn(s, " ");
    if (lead) {
        memmove(s, s + lead, n - lead + 1);
    }
}

void media_genre_clean(char *dst, size_t len, const char *src)
{
    if (!dst || len == 0) {
        return;
    }
    dst[0] = '\0';
    char text[MEDIA_GENRE_LEN * 2];
    text_to_ascii(text, sizeof(text), src ? src : "");
    const char *p = text;
    // ID3: "(17)", "(17)Rock", "17", "(RX)", "(CR)"; a text after the reference wins
    while (*p == '(') {
        const char *close = strchr(p, ')');
        if (!close) {
            break;
        }
        const char *after = close + 1;
        if (*after && *after != '(') {
            p = after;
            break;
        }
        char ref[8] = "";
        size_t n = (size_t)(close - p - 1);
        if (n < sizeof(ref)) {
            snprintf(ref, sizeof(ref), "%.*s", (int)n, p + 1);
        }
        const char *named = NULL;
        if (strcmp(ref, "RX") == 0) {
            named = "Remix";
        } else if (strcmp(ref, "CR") == 0) {
            named = "Cover";
        } else if (small_number(ref, sizeof(ref)) >= 0) {
            named = media_id3_genre(small_number(ref, sizeof(ref)));
        }
        if (named) {
            snprintf(dst, len, "%s", named);
            return;
        }
        p = after;
    }
    size_t left = sizeof(text) - (size_t)(p - text);
    if (small_number(p, left) >= 0) {
        const char *named = media_id3_genre(small_number(p, left));
        snprintf(dst, len, "%s", named ? named : "");
        return;
    }
    // The first of several ("Rock; Pop", "Hip Hop/Rap", "rock, indie")
    char first[MEDIA_GENRE_LEN];
    size_t n = strcspn(p, ";/,|\\");
    snprintf(first, sizeof(first), "%.*s", (int)(n < sizeof(first) ? n : sizeof(first) - 1), p);
    trim(first, sizeof(first));
    for (size_t i = 0; i < sizeof(GENRE_SAME) / sizeof(GENRE_SAME[0]); i++) {
        if (strcasecmp(first, GENRE_SAME[i].from) == 0) {
            snprintf(dst, len, "%s", GENRE_SAME[i].to);
            return;
        }
    }
    for (int i = 0; i < ID3_GENRE_COUNT; i++) {
        if (strcasecmp(first, ID3_GENRES[i]) == 0) {
            snprintf(dst, len, "%s", ID3_GENRES[i]);   // The list's spelling ("r&b" -> "R&B")
            return;
        }
    }
    if (!first[0] || strcasecmp(first, "unknown") == 0 || strcasecmp(first, "genre") == 0 ||
        strcasecmp(first, "none") == 0) {
        return;
    }
    media_fix_case(first);
    if (islower((unsigned char)first[0])) {
        first[0] = (char)toupper((unsigned char)first[0]);
    }
    snprintf(dst, len, "%s", first);
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

static size_t rd(const media_file_t *f, uint64_t off, void *buf, size_t len)
{
    if (off >= f->size || len == 0) {
        return 0;
    }
    if (len > f->size - off) {
        len = (size_t)(f->size - off);
    }
    size_t got = f->read(f->ctx, off, (uint8_t *)buf, len);
    return got > len ? 0 : got;
}

// Exactly len bytes, or false
static bool rd_all(const media_file_t *f, uint64_t off, void *buf, size_t len)
{
    return rd(f, off, buf, len) == len;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[1] << 8 | p[0]); }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p + 4) << 32 | le32(p); }

// ID3v2 "syncsafe" integer: 7 bits per byte; false if a byte has its top bit set
static bool syncsafe(const uint8_t *p, uint32_t *out)
{
    if ((p[0] | p[1] | p[2] | p[3]) & 0x80) {
        return false;
    }
    *out = (uint32_t)p[0] << 21 | (uint32_t)p[1] << 14 | (uint32_t)p[2] << 7 | p[3];
    return true;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

enum { ENC_LATIN1 = 0, ENC_UTF16 = 1, ENC_UTF16BE = 2, ENC_UTF8 = 3 };

static size_t put_utf8(char *out, size_t o, size_t cap, uint32_t cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | cp >> 6); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | cp >> 12); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | cp >> 18); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (o + n >= cap) {
        return o;
    }
    for (size_t i = 0; i < n; i++) {
        out[o + i] = tmp[i];
    }
    return o + n;
}

// The first string of a tag value in encoding `enc`, into dst as printable ASCII
static void put_text(char *dst, size_t len, const uint8_t *src, size_t n, int enc)
{
    char utf8[MEDIA_TEXT_MAX + 4];
    size_t o = 0;
    if (n > MEDIA_TEXT_MAX) {
        n = MEDIA_TEXT_MAX;
    }
    if (enc == ENC_UTF16 || enc == ENC_UTF16BE) {
        bool big = enc == ENC_UTF16BE;
        size_t i = 0;
        if (n >= 2 && src[0] == 0xFF && src[1] == 0xFE) {
            big = false; i = 2;
        } else if (n >= 2 && src[0] == 0xFE && src[1] == 0xFF) {
            big = true; i = 2;
        }
        for (; i + 1 < n; i += 2) {
            uint32_t cp = big ? be16(src + i) : le16(src + i);
            if (cp == 0) {
                break;
            }
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
                uint32_t lo = big ? be16(src + i + 2) : le16(src + i + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            o = put_utf8(utf8, o, sizeof(utf8), cp);
        }
    } else {
        for (size_t i = 0; i < n && src[i]; i++) {
            if (enc != ENC_UTF8) {
                o = put_utf8(utf8, o, sizeof(utf8), src[i]);   // Latin-1: the first 256 code points
            } else if (o + 1 < sizeof(utf8)) {
                utf8[o++] = (char)src[i];
            }
        }
    }
    utf8[o] = '\0';
    text_to_ascii(dst, len, utf8);
}

static void set_if_empty(char *dst, size_t len, const uint8_t *src, size_t n, int enc)
{
    if (!dst[0]) {
        put_text(dst, len, src, n, enc);
    }
}

static uint16_t leading_number(const char *s, size_t n)
{
    uint32_t v = 0;
    size_t i = 0;
    while (i < n && s[i] == ' ') {
        i++;
    }
    size_t start = i;
    while (i < n && i - start < 5 && s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (uint32_t)(s[i] - '0');
        i++;
    }
    return i > start && v <= 65535 ? (uint16_t)v : 0;
}

static void set_year(media_tags_t *t, const char *text, size_t cap)
{
    uint16_t y = leading_number(text, strnlen(text, cap));
    if (!t->year && y >= 1000 && y <= 2999) {
        t->year = y;
    }
}

static void set_track(media_tags_t *t, const char *text, size_t cap)
{
    if (!t->track) {
        t->track = leading_number(text, strnlen(text, cap));   // "3", "03/12"
    }
}

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------

typedef enum { F_NONE, F_TITLE, F_ARTIST, F_ALBUM_ARTIST, F_ALBUM, F_GENRE, F_YEAR, F_TRACK, F_LENGTH } field_t;

static field_t id3_field(const char *id, int version)
{
    static const struct {
        const char *v2;   // ID3v2.2
        const char *v34;  // ID3v2.3 / 2.4
        field_t field;
    } FRAMES[] = {
        {"TT2", "TIT2", F_TITLE}, {"TP1", "TPE1", F_ARTIST}, {"TP2", "TPE2", F_ALBUM_ARTIST},
        {"TAL", "TALB", F_ALBUM}, {"TCO", "TCON", F_GENRE}, {"TYE", "TYER", F_YEAR},
        {"TDA", "TDRC", F_YEAR}, {"TRK", "TRCK", F_TRACK}, {"TLE", "TLEN", F_LENGTH},
    };
    for (size_t i = 0; i < sizeof(FRAMES) / sizeof(FRAMES[0]); i++) {
        if (strcmp(id, version == 2 ? FRAMES[i].v2 : FRAMES[i].v34) == 0) {
            return FRAMES[i].field;
        }
    }
    return F_NONE;
}

// Undoes ID3's unsynchronisation (0xFF 0x00 -> 0xFF) in place; the new length
static size_t unsync(uint8_t *p, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xFF && i + 1 < n && p[i + 1] == 0x00) {
            i++;
        }
    }
    return o;
}

static void apply_field(media_tags_t *t, field_t field, const uint8_t *value, size_t n, int enc, char *album_artist,
                        size_t aa_len)
{
    char text[MEDIA_TITLE_LEN * 2];
    switch (field) {
    case F_TITLE: set_if_empty(t->title, sizeof(t->title), value, n, enc); break;
    case F_ARTIST: set_if_empty(t->artist, sizeof(t->artist), value, n, enc); break;
    case F_ALBUM_ARTIST: set_if_empty(album_artist, aa_len, value, n, enc); break;
    case F_ALBUM: set_if_empty(t->album, sizeof(t->album), value, n, enc); break;
    case F_GENRE:
        if (!t->genre[0]) {
            put_text(text, sizeof(text), value, n, enc);
            media_genre_clean(t->genre, sizeof(t->genre), text);
        }
        break;
    case F_YEAR: put_text(text, sizeof(text), value, n, enc); set_year(t, text, sizeof(text)); break;
    case F_TRACK: put_text(text, sizeof(text), value, n, enc); set_track(t, text, sizeof(text)); break;
    case F_LENGTH:
        put_text(text, sizeof(text), value, n, enc);
        if (!t->duration_ms) {
            long ms = strtol(text, NULL, 10);
            t->duration_ms = ms > 0 && ms < 24L * 3600 * 1000 ? (uint32_t)ms : 0;
        }
        break;
    case F_NONE: break;
    }
}

// The ID3v2 tag at `base`; its total size (header, frames, footer), 0 if there is none
static uint32_t id3v2_read(const media_file_t *f, uint64_t base, media_tags_t *t)
{
    uint8_t h[10];
    uint32_t size = 0;
    if (!rd_all(f, base, h, sizeof(h)) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4 || h[4] == 0xFF ||
        !syncsafe(h + 6, &size)) {
        return 0;
    }
    int version = h[3];
    uint8_t flags = h[5];
    uint32_t total = 10 + size + ((version == 4 && (flags & 0x10)) ? 10 : 0);
    uint64_t end = base + 10 + size;
    if (end > f->size) {
        end = f->size;
    }
    uint64_t pos = base + 10;
    if (flags & 0x40 && version >= 3) {   // Extended header
        uint8_t e[4];
        uint32_t esize = 0;
        if (!rd_all(f, pos, e, 4)) {
            return total;
        }
        if (version == 4) {
            if (!syncsafe(e, &esize)) {
                return total;
            }
            pos += esize;              // Includes its own size field
        } else {
            pos += 4 + be32(e);
        }
    }
    bool tag_unsync = (flags & 0x80) != 0;
    char album_artist[MEDIA_NAME_LEN] = "";
    size_t hdr = version == 2 ? 6 : 10;
    uint8_t value[MEDIA_TEXT_MAX];
    for (int frames = 0; frames < 256 && pos + hdr <= end; frames++) {
        uint8_t fh[10];
        if (!rd_all(f, pos, fh, hdr) || fh[0] == 0) {
            break;   // Padding
        }
        char id[5] = "";
        uint32_t fsize = 0;
        uint16_t fflags = 0;
        if (version == 2) {
            snprintf(id, sizeof(id), "%.3s", (const char *)fh);
            fsize = be24(fh + 3);
        } else {
            snprintf(id, sizeof(id), "%.4s", (const char *)fh);
            if (version == 4) {
                if (!syncsafe(fh + 4, &fsize)) {
                    fsize = be32(fh + 4);   // Some taggers write plain sizes in v2.4
                }
            } else {
                fsize = be32(fh + 4);
            }
            fflags = be16(fh + 8);
        }
        for (size_t i = 0; i < hdr - (version == 2 ? 3 : 6); i++) {
            if (!isupper(fh[i]) && !isdigit(fh[i])) {
                return total;   // Not a frame: the tag ends (or is broken) here
            }
        }
        uint64_t data = pos + hdr;
        if (fsize == 0 || fsize > end - data) {
            break;
        }
        pos = data + fsize;
        field_t field = id3_field(id, version);
        if (field == F_NONE) {
            continue;
        }
        // Compressed or encrypted frames are skipped (v2.3: 0x0080 / 0x0040; v2.4: 0x0008 / 0x0004)
        if ((version == 3 && (fflags & 0x00C0)) || (version == 4 && (fflags & 0x000C))) {
            continue;
        }
        size_t n = fsize < sizeof(value) ? fsize : sizeof(value);
        if (!rd_all(f, data, value, n)) {
            break;
        }
        const uint8_t *v = value;
        if (version == 4 && (fflags & 0x0001)) {   // Data length indicator
            if (n < 4) {
                continue;
            }
            v += 4;
            n -= 4;
        }
        if (tag_unsync || (version == 4 && (fflags & 0x0002))) {
            n = unsync((uint8_t *)v, n);
        }
        if (n < 2 || v[0] > ENC_UTF8) {
            continue;
        }
        apply_field(t, field, v + 1, n - 1, v[0], album_artist, sizeof(album_artist));
    }
    if (!t->artist[0] && album_artist[0]) {
        snprintf(t->artist, sizeof(t->artist), "%s", album_artist);
    }
    return total;
}

// ---------------------------------------------------------------------------
// ID3v1
// ---------------------------------------------------------------------------

static bool id3v1_read(const media_file_t *f, media_tags_t *t)
{
    uint8_t b[128];
    if (f->size < 128 || !rd_all(f, f->size - 128, b, sizeof(b)) || memcmp(b, "TAG", 3) != 0) {
        return false;
    }
    set_if_empty(t->title, sizeof(t->title), b + 3, 30, ENC_LATIN1);
    set_if_empty(t->artist, sizeof(t->artist), b + 33, 30, ENC_LATIN1);
    set_if_empty(t->album, sizeof(t->album), b + 63, 30, ENC_LATIN1);
    char year[5];
    snprintf(year, sizeof(year), "%.4s", (const char *)b + 93);
    set_year(t, year, sizeof(year));
    if (b[125] == 0 && b[126] != 0 && !t->track) {
        t->track = b[126];   // ID3v1.1
    }
    if (!t->genre[0] && media_id3_genre(b[127])) {
        snprintf(t->genre, sizeof(t->genre), "%s", media_id3_genre(b[127]));
    }
    return true;
}

// ---------------------------------------------------------------------------
// MPEG audio frames: the length (Xing / Info / VBRI header, else the bitrate)
// ---------------------------------------------------------------------------

static const uint16_t MP3_KBPS[2][3][16] = {
    {   // MPEG-1: layer I, II, III
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
    },
    {   // MPEG-2 / 2.5
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
    },
};
static const uint32_t MP3_RATES[3] = {44100, 48000, 32000};

static void mp3_length(const media_file_t *f, uint64_t start, bool has_v1, media_tags_t *t)
{
    uint8_t b[2048];
    size_t n = rd(f, start, b, sizeof(b));
    for (size_t i = 0; i + 4 <= n; i++) {
        if (b[i] != 0xFF || (b[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        int ver = (b[i + 1] >> 3) & 3;     // 0: 2.5, 2: 2, 3: 1
        int layer = (b[i + 1] >> 1) & 3;   // 1: III, 2: II, 3: I
        int br = b[i + 2] >> 4;
        int sr = (b[i + 2] >> 2) & 3;
        if (ver == 1 || layer == 0 || br == 0 || br == 15 || sr == 3) {
            continue;
        }
        bool v1 = ver == 3;
        int li = 3 - layer;   // 0: I, 1: II, 2: III
        uint32_t rate = MP3_RATES[sr] >> (v1 ? 0 : ver == 2 ? 1 : 2);
        uint32_t kbps = MP3_KBPS[v1 ? 0 : 1][li][br];
        uint32_t spf = li == 0 ? 384 : (li == 2 && !v1) ? 576 : 1152;
        bool mono = (b[i + 3] >> 6) == 3;
        size_t side = li != 2 ? 0 : v1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        size_t x = i + 4 + side;
        uint32_t frames = 0;
        if (x + 12 <= n && (memcmp(b + x, "Xing", 4) == 0 || memcmp(b + x, "Info", 4) == 0) && (be32(b + x + 4) & 1)) {
            frames = be32(b + x + 8);
        } else if (i + 36 + 18 <= n && memcmp(b + i + 36, "VBRI", 4) == 0) {
            frames = be32(b + i + 36 + 14);
        }
        uint64_t ms = 0;
        if (frames) {
            ms = (uint64_t)frames * spf * 1000 / rate;
        } else {
            uint64_t bytes = f->size - start - (uint64_t)i - (has_v1 ? 128 : 0);
            ms = bytes * 8 / kbps;   // Constant bitrate: bytes * 8 / (kbps * 1000) seconds
        }
        if (!t->duration_ms && ms > 0 && ms < 24ULL * 3600 * 1000) {
            t->duration_ms = (uint32_t)ms;
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// Vorbis comments (FLAC, Ogg Vorbis, Opus)
// ---------------------------------------------------------------------------

static void vorbis_comments(const uint8_t *p, size_t n, media_tags_t *t)
{
    if (n < 8) {
        return;
    }
    uint32_t vendor = le32(p);
    if (vendor > n - 8) {
        return;
    }
    size_t pos = 4 + vendor;
    uint32_t count = le32(p + pos);
    pos += 4;
    char album_artist[MEDIA_NAME_LEN] = "";
    for (uint32_t i = 0; i < count && i < 256 && pos + 4 <= n; i++) {
        uint32_t len = le32(p + pos);
        pos += 4;
        if (len > n - pos) {
            break;   // Truncated (the block is read up to MEDIA_COMMENTS_MAX)
        }
        const uint8_t *e = p + pos;
        pos += len;
        const uint8_t *eq = memchr(e, '=', len);
        if (!eq) {
            continue;
        }
        size_t klen = (size_t)(eq - e);
        const uint8_t *v = eq + 1;
        size_t vlen = len - klen - 1;
        char key[16];
        if (klen == 0 || klen >= sizeof(key)) {
            continue;
        }
        snprintf(key, sizeof(key), "%.*s", (int)klen, (const char *)e);
        field_t field = F_NONE;
        if (strcasecmp(key, "TITLE") == 0) {
            field = F_TITLE;
        } else if (strcasecmp(key, "ARTIST") == 0) {
            field = F_ARTIST;
        } else if (strcasecmp(key, "ALBUMARTIST") == 0 || strcasecmp(key, "ALBUM ARTIST") == 0) {
            field = F_ALBUM_ARTIST;
        } else if (strcasecmp(key, "ALBUM") == 0) {
            field = F_ALBUM;
        } else if (strcasecmp(key, "GENRE") == 0) {
            field = F_GENRE;
        } else if (strcasecmp(key, "DATE") == 0 || strcasecmp(key, "YEAR") == 0) {
            field = F_YEAR;
        } else if (strcasecmp(key, "TRACKNUMBER") == 0) {
            field = F_TRACK;
        }
        apply_field(t, field, v, vlen, ENC_UTF8, album_artist, sizeof(album_artist));
    }
    if (!t->artist[0] && album_artist[0]) {
        snprintf(t->artist, sizeof(t->artist), "%s", album_artist);
    }
}

// ---------------------------------------------------------------------------
// FLAC
// ---------------------------------------------------------------------------

static bool flac_read(const media_file_t *f, uint64_t start, media_tags_t *t)
{
    uint8_t h[4];
    if (!rd_all(f, start, h, 4) || memcmp(h, "fLaC", 4) != 0) {
        return false;
    }
    t->audio_offset = (uint32_t)start;   // The decoder needs the "fLaC" header first
    uint64_t pos = start + 4;
    for (int blocks = 0; blocks < 64; blocks++) {
        uint8_t bh[4];
        if (!rd_all(f, pos, bh, 4)) {
            break;
        }
        bool last = (bh[0] & 0x80) != 0;
        int type = bh[0] & 0x7F;
        uint32_t len = be24(bh + 1);
        uint64_t data = pos + 4;
        if (len > f->size - data) {
            break;
        }
        if (type == 0 && len >= 18) {   // STREAMINFO
            uint8_t s[18];
            if (rd_all(f, data, s, sizeof(s))) {
                uint32_t rate = (uint32_t)s[10] << 12 | (uint32_t)s[11] << 4 | s[12] >> 4;
                uint64_t samples = (uint64_t)(s[13] & 0x0F) << 32 | be32(s + 14);
                if (rate && samples) {
                    t->duration_ms = (uint32_t)(samples * 1000 / rate);
                }
            }
        } else if (type == 4) {         // VORBIS_COMMENT
            size_t n = len < MEDIA_COMMENTS_MAX ? len : MEDIA_COMMENTS_MAX;
            uint8_t *buf = malloc(n);
            if (buf && rd_all(f, data, buf, n)) {
                vorbis_comments(buf, n, t);
            }
            free(buf);
        }
        pos = data + len;
        if (last) {
            break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Ogg (Vorbis, Opus)
// ---------------------------------------------------------------------------

static bool ogg_read(const media_file_t *f, media_tags_t *t)
{
    // The first two packets of the first stream: the identification and the comment header
    uint8_t *pkt = malloc(MEDIA_COMMENTS_MAX);
    if (!pkt) {
        return false;
    }
    size_t plen = 0;
    int packet = 0;
    bool opus = false, ok = false;
    uint32_t rate = 0, serial = 0;
    uint16_t preskip = 0;
    uint64_t pos = 0;
    for (int pages = 0; pages < 64 && packet < 2; pages++) {
        uint8_t ph[27 + 255];
        if (!rd_all(f, pos, ph, 27) || memcmp(ph, "OggS", 4) != 0 || ph[4] != 0) {
            break;
        }
        uint32_t page_serial = le32(ph + 14);
        if (pages == 0) {
            serial = page_serial;
        }
        int segs = ph[26];
        if (!rd_all(f, pos + 27, ph + 27, (size_t)segs)) {
            break;
        }
        uint64_t data = pos + 27 + (uint64_t)segs;
        uint64_t body = 0;
        for (int i = 0; i < segs; i++) {
            body += ph[27 + i];
        }
        if (body > f->size - data) {
            break;
        }
        if (page_serial == serial) {
            uint64_t off = data;
            for (int i = 0; i < segs && packet < 2; i++) {
                size_t seg = ph[27 + i];
                size_t take = plen + seg <= MEDIA_COMMENTS_MAX ? seg : MEDIA_COMMENTS_MAX - plen;
                if (take && !rd_all(f, off, pkt + plen, take)) {
                    take = 0;
                }
                plen += take;
                off += seg;
                if (seg < 255) {   // The packet ends here
                    if (packet == 0) {
                        if (plen >= 16 && memcmp(pkt, "\x01vorbis", 7) == 0) {
                            rate = le32(pkt + 12);
                            ok = true;
                        } else if (plen >= 19 && memcmp(pkt, "OpusHead", 8) == 0) {
                            opus = true;
                            preskip = le16(pkt + 10);
                            rate = 48000;   // Opus granule positions always count 48 kHz samples
                            ok = true;
                        } else {
                            break;          // Not Vorbis or Opus (e.g. FLAC in Ogg)
                        }
                    } else if (!opus && plen >= 7 && memcmp(pkt, "\x03vorbis", 7) == 0) {
                        vorbis_comments(pkt + 7, plen - 7, t);
                    } else if (opus && plen >= 8 && memcmp(pkt, "OpusTags", 8) == 0) {
                        vorbis_comments(pkt + 8, plen - 8, t);
                    }
                    packet++;
                    plen = 0;
                }
            }
            if (!ok) {
                break;
            }
        }
        pos = data + body;
    }
    free(pkt);
    if (!ok) {
        return false;
    }
    t->format = opus ? MEDIA_OPUS : MEDIA_VORBIS;
    // The length: the granule position of the stream's last page
    size_t window = f->size < MEDIA_OGG_TAIL ? (size_t)f->size : MEDIA_OGG_TAIL;
    uint8_t *tail = malloc(window);
    if (tail && rate && window >= 27 && rd_all(f, f->size - window, tail, window)) {
        for (size_t i = window - 27 + 1; i-- > 0;) {   // Backwards from the last possible page header
            if (memcmp(tail + i, "OggS", 4) == 0 && le32(tail + i + 14) == serial) {
                uint64_t granule = le64(tail + i + 6);
                if (granule != UINT64_MAX && granule > preskip) {
                    uint64_t ms = (granule - preskip) * 1000 / rate;
                    t->duration_ms = ms < 24ULL * 3600 * 1000 ? (uint32_t)ms : 0;
                }
                break;
            }
        }
    }
    free(tail);
    return true;
}

// ---------------------------------------------------------------------------
// MP4 / M4A
// ---------------------------------------------------------------------------

typedef struct {
    const media_file_t *f;
    media_tags_t *t;
    int boxes;              // Boxes visited (bounded)
    bool seen_moov;
    bool mdat_first;        // The audio comes before the index: the decoder can't stream it
    char album_artist[MEDIA_NAME_LEN];
} mp4_t;

static bool box_is(const uint8_t *type, const char *name)
{
    return memcmp(type, name, 4) == 0;
}

// One iTunes item (moov/udta/meta/ilst/<item>): its "data" box holds the value
static void mp4_item(mp4_t *m, const uint8_t *type, uint64_t pos, uint64_t end)
{
    uint8_t h[16];
    if (end - pos < 16 || !rd_all(m->f, pos, h, 16) || !box_is(h + 4, "data")) {
        return;
    }
    uint64_t dsize = be32(h);
    if (dsize < 16 || dsize > end - pos) {
        return;
    }
    uint32_t kind = be32(h + 8) & 0xFFFFFF;   // 1: UTF-8, 0: binary
    size_t n = (size_t)(dsize - 16) < MEDIA_TEXT_MAX ? (size_t)(dsize - 16) : MEDIA_TEXT_MAX;
    uint8_t v[MEDIA_TEXT_MAX];
    if (!rd_all(m->f, pos + 16, v, n)) {
        return;
    }
    media_tags_t *t = m->t;
    if (box_is(type, "trkn")) {
        if (n >= 4 && !t->track) {
            t->track = be16(v + 2);
        }
        return;
    }
    if (box_is(type, "gnre")) {
        if (n >= 2 && !t->genre[0] && media_id3_genre(be16(v) - 1)) {
            snprintf(t->genre, sizeof(t->genre), "%s", media_id3_genre(be16(v) - 1));
        }
        return;
    }
    if (kind != 1) {
        return;
    }
    field_t field = F_NONE;
    if (box_is(type, "\xA9nam")) {
        field = F_TITLE;
    } else if (box_is(type, "\xA9" "ART")) {
        field = F_ARTIST;
    } else if (box_is(type, "aART")) {
        field = F_ALBUM_ARTIST;
    } else if (box_is(type, "\xA9" "alb")) {
        field = F_ALBUM;
    } else if (box_is(type, "\xA9gen")) {
        field = F_GENRE;
    } else if (box_is(type, "\xA9" "day")) {
        field = F_YEAR;
    }
    apply_field(t, field, v, n, ENC_UTF8, m->album_artist, sizeof(m->album_artist));
}

static void mp4_boxes(mp4_t *m, uint64_t pos, uint64_t end, int depth, const uint8_t *parent)
{
    while (depth < 8 && m->boxes < 512 && end - pos >= 8) {
        m->boxes++;
        uint8_t h[16];
        if (!rd_all(m->f, pos, h, 8)) {
            return;
        }
        uint64_t size = be32(h);
        uint64_t hdr = 8;
        if (size == 1) {
            if (end - pos < 16 || !rd_all(m->f, pos + 8, h + 8, 8)) {
                return;
            }
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = end - pos;   // To the end of the file
        }
        if (size < hdr || size > end - pos) {
            return;
        }
        const uint8_t *type = h + 4;
        uint64_t body = pos + hdr, next = pos + size;
        if (depth == 0 && box_is(type, "mdat") && !m->seen_moov) {
            m->mdat_first = true;
        } else if (depth == 0 && box_is(type, "moov")) {
            m->seen_moov = true;
        }
        if (box_is(type, "moov") || box_is(type, "udta") || box_is(type, "ilst") || box_is(type, "trak") ||
            box_is(type, "mdia") || box_is(type, "minf") || box_is(type, "stbl")) {
            mp4_boxes(m, body, next, depth + 1, type);
        } else if (box_is(type, "meta")) {
            // A full box (4 bytes of version and flags), except in some QuickTime files
            uint8_t peek[8];
            uint64_t skip = rd_all(m->f, body, peek, 8) && !box_is(peek + 4, "hdlr") ? 4 : 0;
            mp4_boxes(m, body + skip, next, depth + 1, type);
        } else if (box_is(type, "mvhd")) {
            // Version 0: 32-bit times, the time scale at 12 and the duration at 16; version 1:
            // 64-bit times, at 20 and 24
            uint8_t v[32];
            if (size - hdr >= 20 && rd_all(m->f, body, v, 20)) {
                bool wide = v[0] == 1;
                uint32_t scale = 0;
                uint64_t dur = 0;
                if (!wide) {
                    scale = be32(v + 12);
                    dur = be32(v + 16);
                } else if (size - hdr >= 32 && rd_all(m->f, body, v, 32)) {
                    scale = be32(v + 20);
                    dur = be64(v + 24);
                }
                if (scale && dur && !m->t->duration_ms) {
                    uint64_t ms = dur * 1000 / scale;
                    m->t->duration_ms = ms < 24ULL * 3600 * 1000 ? (uint32_t)ms : 0;
                }
            }
        } else if (box_is(type, "stsd")) {
            uint8_t v[16];
            if (size - hdr >= 16 && rd_all(m->f, body, v, sizeof(v)) && box_is(v + 12, "alac")) {
                m->t->format = MEDIA_ALAC;
            }
        } else if (parent && box_is(parent, "ilst")) {
            mp4_item(m, type, body, next);
        }
        pos = next;
    }
}

static bool mp4_read(const media_file_t *f, media_tags_t *t)
{
    uint8_t h[8];
    if (!rd_all(f, 0, h, 8) || !box_is(h + 4, "ftyp")) {
        return false;
    }
    mp4_t m = {.f = f, .t = t};
    mp4_boxes(&m, 0, f->size, 0, NULL);
    if (!t->artist[0] && m.album_artist[0]) {
        snprintf(t->artist, sizeof(t->artist), "%s", m.album_artist);
    }
    t->playable = m.seen_moov && !m.mdat_first;
    return true;
}

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------

static bool wav_read(const media_file_t *f, media_tags_t *t)
{
    uint8_t h[12];
    if (!rd_all(f, 0, h, 12) || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        return false;
    }
    uint32_t byte_rate = 0;
    uint64_t data_size = 0;
    uint64_t pos = 12;
    char album_artist[MEDIA_NAME_LEN] = "";
    for (int chunks = 0; chunks < 64 && f->size - pos >= 8; chunks++) {
        uint8_t ch[8];
        if (!rd_all(f, pos, ch, 8)) {
            break;
        }
        uint64_t len = le32(ch + 4);
        uint64_t body = pos + 8;
        if (len > f->size - body) {
            len = f->size - body;   // A "data" chunk of a recording that was cut short
        }
        if (memcmp(ch, "fmt ", 4) == 0 && len >= 16) {
            uint8_t fmt[16];
            if (rd_all(f, body, fmt, sizeof(fmt))) {
                uint16_t tag = le16(fmt);
                byte_rate = le32(fmt + 8);
                t->playable = tag != 3;   // IEEE float: the decoder plays integer PCM, ADPCM and G.711
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            data_size = len;
        } else if (memcmp(ch, "LIST", 4) == 0 && len >= 4) {
            uint8_t kind[4];
            if (rd_all(f, body, kind, 4) && memcmp(kind, "INFO", 4) == 0) {
                uint64_t p = body + 4, end = body + len;
                for (int items = 0; items < 32 && end - p >= 8; items++) {
                    uint8_t ih[8];
                    if (!rd_all(f, p, ih, 8)) {
                        break;
                    }
                    uint64_t ilen = le32(ih + 4);
                    if (ilen > end - p - 8) {
                        break;
                    }
                    uint8_t v[MEDIA_TEXT_MAX];
                    size_t n = ilen < sizeof(v) ? (size_t)ilen : sizeof(v);
                    field_t field = memcmp(ih, "INAM", 4) == 0 ? F_TITLE
                                    : memcmp(ih, "IART", 4) == 0 ? F_ARTIST
                                    : memcmp(ih, "IPRD", 4) == 0 ? F_ALBUM
                                    : memcmp(ih, "IGNR", 4) == 0 ? F_GENRE
                                    : memcmp(ih, "ICRD", 4) == 0 ? F_YEAR
                                    : memcmp(ih, "ITRK", 4) == 0 || memcmp(ih, "IPRT", 4) == 0 ? F_TRACK
                                    : F_NONE;
                    if (field != F_NONE && rd_all(f, p + 8, v, n)) {
                        apply_field(t, field, v, n, ENC_UTF8, album_artist, sizeof(album_artist));
                    }
                    p += 8 + ilen + (ilen & 1);
                }
            }
        } else if (memcmp(ch, "id3 ", 4) == 0 || memcmp(ch, "ID3 ", 4) == 0) {
            id3v2_read(f, body, t);
        }
        pos = body + len + (len & 1);   // Chunks are padded to even sizes
    }
    if (byte_rate && data_size && !t->duration_ms) {
        uint64_t ms = data_size * 1000 / byte_rate;
        t->duration_ms = ms < 24ULL * 3600 * 1000 ? (uint32_t)ms : 0;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

bool media_tags_read(const media_file_t *file, media_format_t format, media_tags_t *out)
{
    static const media_tags_t EMPTY;
    *out = EMPTY;
    out->format = format;
    out->playable = true;
    if (!file || !file->read || file->size == 0 || format == MEDIA_NONE || format >= MEDIA_FORMATS) {
        return false;
    }
    switch (format) {
    case MEDIA_MP3:
    case MEDIA_AAC: {
        uint32_t id3 = id3v2_read(file, 0, out);
        out->audio_offset = id3 < file->size ? id3 : 0;
        bool v1 = id3v1_read(file, out);
        if (format == MEDIA_MP3) {
            mp3_length(file, out->audio_offset, v1, out);
        }
        return true;
    }
    case MEDIA_FLAC: {
        uint32_t id3 = id3v2_read(file, 0, out);   // Some taggers put ID3 in front of a FLAC file
        return flac_read(file, id3, out);
    }
    case MEDIA_VORBIS:
    case MEDIA_OPUS:
        return ogg_read(file, out);
    case MEDIA_M4A:
    case MEDIA_ALAC:
        return mp4_read(file, out);
    case MEDIA_WAV:
        return wav_read(file, out);
    case MEDIA_AMR:
    case MEDIA_AMRWB: {
        uint8_t h[9];
        size_t n = rd(file, 0, h, sizeof(h));
        return format == MEDIA_AMR ? n >= 6 && memcmp(h, "#!AMR\n", 6) == 0
                                   : n >= 9 && memcmp(h, "#!AMR-WB\n", 9) == 0;
    }
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Names from the path
// ---------------------------------------------------------------------------

// "CD1", "Disc 2", "disk-3": a folder of one disc of an album (name is in a 256-byte path)
static bool disc_folder(const char *name, size_t cap)
{
    static const struct {
        const char *prefix;
        size_t len;
    } PREFIXES[] = {{"cd", 2}, {"disc", 4}, {"disk", 4}};
    for (size_t i = 0; i < sizeof(PREFIXES) / sizeof(PREFIXES[0]); i++) {
        size_t n = PREFIXES[i].len;
        if (strncasecmp(name, PREFIXES[i].prefix, n) == 0) {
            const char *p = name + n;
            p += strspn(p, " _-");
            return *p && strspn(p, "0123456789") == strnlen(p, cap);
        }
    }
    return false;
}

// "01 - Title", "01. Title", "1-03 Title", "(01) Title", "07 Title": the number is taken out and
// returned. A number that may belong to the title stays: "99 Luftballons", "1979", "24".
// s is a buffer of `cap` bytes.
static uint16_t strip_track_number(char *s, size_t cap)
{
    char *p = s;
    bool bracket = *p == '(' || *p == '[';
    p += bracket;
    size_t digits = strspn(p, "0123456789");
    if (digits == 0 || digits > 3) {
        return 0;
    }
    char *after = p + digits;
    bool disc = false;
    if (*after == '-' && isdigit((unsigned char)after[1]) && digits == 1) {   // Disc-track: "1-03"
        p = after + 1;
        digits = strspn(p, "0123456789");
        if (digits == 0 || digits > 3) {
            return 0;
        }
        after = p + digits;
        disc = true;
    }
    if (bracket) {
        if (*after != ')' && *after != ']') {
            return 0;
        }
        after++;
    }
    uint16_t n = leading_number(p, digits);
    size_t sep = strspn(after, " .-)");
    bool marked = bracket || disc || after[0] == '.' || after[0] == '-' || after[0] == ')' ||
                  strncmp(after, " - ", 3) == 0 || strncmp(after, " . ", 3) == 0;
    // Only a space after it: a track number when it looks like one (01-39, or 101-939 with
    // the disc in front)
    bool spaced = after[0] == ' ' && ((digits == 2 && n >= 1 && n <= 39) ||
                                      (digits == 3 && p[0] != '0' && n % 100 >= 1 && n % 100 <= 39));
    if ((!marked && !spaced) || !after[sep]) {
        return 0;
    }
    if (digits == 3 && !disc) {
        n = n % 100;
    }
    size_t rest = strnlen(after + sep, cap - (size_t)(after + sep - s));
    memmove(s, after + sep, rest + 1);
    return n;
}

// At most len - 1 characters of src: names from the path are cut to fit, on purpose
static void copy_text(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    for (; i + 1 < len && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void fill(char *dst, size_t len, const char *src)
{
    if (!dst[0] && src && src[0]) {
        copy_text(dst, len, src);
        trim(dst, len);
    }
}

void media_tags_from_path(const char *rel_path, media_tags_t *t)
{
    char path[256];
    text_to_ascii(path, sizeof(path), rel_path ? rel_path : "");
    for (char *p = path; *p; p++) {
        if (*p == '_') {
            *p = ' ';
        }
    }
    // Split at '/': the file and the folders above it (the last few are kept)
    char *parts[8];
    int count = 0;
    for (char *p = path; *p;) {
        char *slash = strchr(p, '/');
        if (slash) {
            *slash = '\0';
        }
        if (*p) {
            if (count == 8) {
                memmove(parts, parts + 1, sizeof(parts) - sizeof(parts[0]));
                count--;
            }
            parts[count++] = p;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    char *file = count ? parts[count - 1] : path;
    int folders = count - 1;
    if (folders > 0 && disc_folder(parts[folders - 1], sizeof(path) - (size_t)(parts[folders - 1] - path))) {
        folders--;   // "Album/CD1/track": the album is the folder above
    }
    char *parent = folders >= 1 ? parts[folders - 1] : NULL;
    char *grand = folders >= 2 ? parts[folders - 2] : NULL;

    char base[MEDIA_TITLE_LEN * 2];
    copy_text(base, sizeof(base), file);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        *dot = '\0';
    }
    trim(base, sizeof(base));
    uint16_t number = strip_track_number(base, sizeof(base));
    if (!t->track) {
        t->track = number;
    }
    // "Artist - Title" or "Artist - Album - Title"
    char *dash = strstr(base, " - ");
    if (dash) {
        *dash = '\0';
        char *rest = dash + 3;
        char *second = strstr(rest, " - ");
        if (second) {
            *second = '\0';
            fill(t->album, sizeof(t->album), rest);
            rest = second + 3;
        }
        if (strspn(base, "0123456789 ") != strnlen(base, sizeof(base))) {   // Not "01 - Artist - Title"'s number
            fill(t->artist, sizeof(t->artist), base);
        }
        fill(t->title, sizeof(t->title), rest);
    }
    fill(t->title, sizeof(t->title), dash ? dash + 3 : base);
    if (!t->title[0]) {
        fill(t->title, sizeof(t->title), file);
    }
    // Folders: "Artist/Album/..." or "Artist - Album/..."
    if (parent) {
        char *pd = strstr(parent, " - ");
        if (pd && !grand) {
            *pd = '\0';
            fill(t->artist, sizeof(t->artist), parent);
            fill(t->album, sizeof(t->album), pd + 3);
        } else {
            fill(t->album, sizeof(t->album), parent);
            if (grand) {
                fill(t->artist, sizeof(t->artist), grand);
            }
        }
    }
    media_fix_case(t->title);
    media_fix_case(t->artist);
    media_fix_case(t->album);
}
