// SPDX-License-Identifier: Apache-2.0
//
// Key values from board_read_key(): printable ASCII as itself, a few control characters, and
// KEY_* codes (>= 0x100) for keys without a character. No ESP-IDF headers (host tests).

#pragma once

#include <stdint.h>

#define KEY_BACKSPACE  8
#define KEY_TAB        '\t'
#define KEY_ENTER      '\n'
#define KEY_ESC        27

#define KEY_UP         0x101
#define KEY_DOWN       0x102
#define KEY_LEFT       0x103
#define KEY_RIGHT      0x104
#define KEY_HOME       0x105   // Start of the line / first row
#define KEY_END        0x106
#define KEY_PAGE_UP    0x107
#define KEY_PAGE_DOWN  0x108
#define KEY_DELETE     0x109   // Delete at the cursor
#define KEY_GO_HOME    0x10A   // Back to Now Playing from anywhere (Tab5: Ctrl+Del)

// Media keys (a Bluetooth keyboard's or remote's): act on the player from any screen
#define KEY_PLAY_PAUSE 0x110
#define KEY_NEXT_TRACK 0x111
#define KEY_PREV_TRACK 0x112
#define KEY_VOL_UP     0x113
#define KEY_VOL_DOWN   0x114
#define KEY_MUTE       0x115

static inline int key_is_char(uint32_t key)
{
    return key >= 0x20 && key < 0x7F;
}
