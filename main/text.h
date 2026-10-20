// SPDX-License-Identifier: Apache-2.0
//
// Text from the network (station names, song titles, SSIDs) made safe for the screen. The UI
// fonts cover printable ASCII only, so UTF-8 is decoded and transliterated: typographic
// punctuation to its ASCII look-alike, accented Latin letters to their base letter, anything
// else to one '?'. Control characters never reach the screen. No ESP-IDF headers (host tests).

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes at most dstlen-1 characters and a NUL. Leading/trailing spaces are dropped and runs
// of whitespace collapse to one space.
void text_to_ascii(char *dst, size_t dstlen, const char *src);

#ifdef __cplusplus
}
#endif
