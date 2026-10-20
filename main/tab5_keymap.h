// SPDX-License-Identifier: Apache-2.0
//
// Tab5 Keymap: turns the M5Stack Tab5 Keyboard's raw key presses and releases (row 0-4,
// column 0-13) into key values (keys.h). The keyboard firmware reports only positions; this
// does the legends, the Sym layer, Aa (hold = shift, tap = one capital, double tap = caps
// lock), Ctrl and auto-repeat. Ported from SimplrSSH's tab5_keymap.cpp. No ESP-IDF headers:
// tested on the host.
//
//   Up / Down / Left / Right   KEY_UP ... KEY_RIGHT
//   Sym+Left / Sym+Right       KEY_HOME / KEY_END
//   Sym+Up / Sym+Down          KEY_PAGE_UP / KEY_PAGE_DOWN
//   Del                        KEY_DELETE;  Ctrl+Del: KEY_GO_HOME (Now Playing)

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_KEYMAP_ROWS 5
#define TAB5_KEYMAP_COLS 14
#define TAB5_KEYMAP_QUEUE 16

typedef struct {
    uint32_t queue[TAB5_KEYMAP_QUEUE];
    int head;
    int count;

    bool sym, ctrl;
    bool aa_down, aa_used, one_shot, caps_lock;
    uint32_t aa_pressed_ms, aa_last_tap_ms;

    int repeat_key;            // row * COLS + col of the auto-repeating key, -1 = none
    uint32_t repeat_value;
    uint32_t repeat_next_ms;
} tab5_keymap_t;

void tab5_keymap_reset(tab5_keymap_t *km);   // Also the initial state
void tab5_keymap_event(tab5_keymap_t *km, bool pressed, int row, int col, uint32_t now_ms);
void tab5_keymap_tick(tab5_keymap_t *km, uint32_t now_ms);   // Auto-repeat
uint32_t tab5_keymap_pop(tab5_keymap_t *km);                 // Next key, 0 if none

#ifdef __cplusplus
}
#endif
