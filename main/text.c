// SPDX-License-Identifier: Apache-2.0
//
// UTF-8 to screen-safe ASCII (text.h).

#include "text.h"
#include <stdint.h>
#include <string.h>

// Base letters for U+00C0..U+00FF and U+0100..U+017F; '*' = several letters (multi() below)
static const char LATIN1[64 + 1] =
    "AAAAAA*CEEEEIIIIDNOOOOOxOUUUUY**"
    "aaaaaa*ceeeeiiiidnooooo/ouuuuy*y";
static const char LATIN_EXT_A[128 + 1] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGg"
    "GgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSs"
    "SsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

static const char *multi(uint32_t cp)
{
    switch (cp) {
    case 0xC6: return "AE";
    case 0xE6: return "ae";
    case 0xDE: return "Th";
    case 0xFE: return "th";
    case 0xDF: return "ss";
    case 0x2026: return "...";
    default: return "?";
    }
}

// The ASCII for one code point: a single character in *one, or a string returned
static const char *fold(uint32_t cp, char *one)
{
    *one = 0;
    if (cp >= 0x20 && cp <= 0x7E) {
        *one = (char)cp;
    } else if (cp < 0x20 || cp == 0x7F || cp == 0xA0 || cp == 0x2028 || cp == 0x2029 ||
               (cp >= 0x2000 && cp <= 0x200A) || cp == 0x3000) {
        *one = ' ';   // Controls (incl. tab, newline) and Unicode spaces
    } else if (cp == 0x200B || cp == 0xFEFF || cp == 0xAD) {
        return "";    // Zero-width space, byte-order mark, soft hyphen
    } else if (cp >= 0xC0 && cp <= 0xFF) {
        *one = LATIN1[cp - 0xC0];
    } else if (cp >= 0x100 && cp <= 0x17F) {
        *one = LATIN_EXT_A[cp - 0x100];
    } else {
        switch (cp) {
        case 0x2018: case 0x2019: case 0x201A: case 0x02BC: case 0x2032: case 0xB4:
            *one = '\''; break;
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0xAB: case 0xBB:
            *one = '"'; break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            *one = '-'; break;
        case 0x2022: case 0xB7:
            *one = '*'; break;
        default:
            break;
        }
    }
    if (*one == '*' && cp >= 0xC0 && cp <= 0xFF) {
        *one = 0;   // AE, sharp s, thorn ... spell several letters
    }
    return *one ? NULL : multi(cp);
}

void text_to_ascii(char *dst, size_t dstlen, const char *src)
{
    if (!dst || dstlen == 0) {
        return;
    }
    size_t o = 0;
    int pending_space = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p) {
        unsigned char c = *p;
        uint32_t cp;
        int n;
        if (c < 0x80) {
            cp = c; n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07; n = 4;
        } else {
            cp = 0xFFFD; n = 1;   // Stray continuation byte
        }
        for (int i = 1; i < n && cp != 0xFFFD; i++) {
            if ((p[i] & 0xC0) != 0x80) {
                cp = 0xFFFD;      // Truncated sequence: consume only the lead byte
                n = 1;
                break;
            }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        p += n;

        char one;
        const char *rep = fold(cp, &one);
        if (!rep && one == ' ') {
            pending_space = o > 0;   // Collapse runs; no leading space
            continue;
        }
        if (rep && !*rep) {
            continue;
        }
        if (pending_space) {
            if (o + 1 >= dstlen) {
                break;
            }
            dst[o++] = ' ';
            pending_space = 0;
        }
        if (!rep) {
            if (o + 1 >= dstlen) {
                break;
            }
            dst[o++] = one;
        } else {
            // noopengrep: mh.gitlab.c_buffer_rule-strlen-wcslen - rep is one of this file's replacement string literals
            size_t n = strlen(rep);   // Whole or not at all ("..." never becomes ".")
            if (o + n >= dstlen) {
                break;
            }
            // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - o + n < dstlen is checked just above
            memcpy(dst + o, rep, n);
            o += n;
        }
    }
    dst[o] = '\0';   // A trailing space is never written: it stays pending
}
