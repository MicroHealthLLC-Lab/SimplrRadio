// SPDX-License-Identifier: Apache-2.0
//
// Tab5 Keymap implementation. Legends from the keyboard's own firmware
// (M5Tab5-Keyboard-Internal-FW, user_keyboard_handle.c): BASE is the printed key, SYM the
// key's Sym-layer legend (0 = none).

#include "tab5_keymap.h"
#include "keys.h"
#include <string.h>

#define REPEAT_DELAY_MS    450
#define REPEAT_INTERVAL_MS 50
#define DOUBLE_TAP_MS      500

static const char BASE[TAB5_KEYMAP_ROWS][TAB5_KEYMAP_COLS] = {
    {0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', 0},
    {'`', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '[', ']', '\\'},
    {0, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ';', '\'', 0},
    {0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0, '_', 0},
    {0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', 0, 0, 0, ' '},
};
static const char SYM[TAB5_KEYMAP_ROWS][TAB5_KEYMAP_COLS] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {'~', '?', 0, 0, 0, 0, 0, 0, '/', '<', '>', '{', '}', '|'},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '"', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '=', 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, ',', 0, 0, 0, 0},
};

static bool at(int row, int col, int r, int c) { return row == r && col == c; }

// a - b >= 0 for wrapping millisecond counters
static bool reached(uint32_t now, uint32_t when) { return (int32_t)(now - when) >= 0; }

static void push(tab5_keymap_t *km, uint32_t value)
{
    if (value && km->count < TAB5_KEYMAP_QUEUE) {
        km->queue[(km->head + km->count) % TAB5_KEYMAP_QUEUE] = value;
        km->count++;
    }
}

uint32_t tab5_keymap_pop(tab5_keymap_t *km)
{
    if (km->count == 0) {
        return 0;
    }
    uint32_t value = km->queue[km->head];
    km->head = (km->head + 1) % TAB5_KEYMAP_QUEUE;
    km->count--;
    return value;
}

void tab5_keymap_reset(tab5_keymap_t *km)
{
    *km = (tab5_keymap_t){0};
    km->repeat_key = -1;
}

static uint32_t translate(tab5_keymap_t *km, int row, int col, bool *repeats)
{
    *repeats = true;
    if (at(row, col, 0, 0)) { *repeats = false; return KEY_ESC; }
    if (at(row, col, 2, 0)) { *repeats = false; return KEY_TAB; }
    if (at(row, col, 3, 13)) { *repeats = false; return KEY_ENTER; }
    if (at(row, col, 2, 13)) { return KEY_BACKSPACE; }
    if (at(row, col, 0, 13)) {
        if (km->ctrl) { *repeats = false; return KEY_GO_HOME; }
        return KEY_DELETE;
    }
    if (at(row, col, 3, 11)) { return km->sym ? KEY_PAGE_UP : KEY_UP; }
    if (at(row, col, 4, 11)) { return km->sym ? KEY_PAGE_DOWN : KEY_DOWN; }
    if (at(row, col, 4, 10)) { return km->sym ? KEY_HOME : KEY_LEFT; }
    if (at(row, col, 4, 12)) { return km->sym ? KEY_END : KEY_RIGHT; }

    char c = (km->sym && SYM[row][col]) ? SYM[row][col] : BASE[row][col];
    if (c >= 'a' && c <= 'z') {
        if (km->ctrl) {
            *repeats = false;
            return (uint32_t)(c - 'a' + 1);   // Ctrl+A = 1 ... Ctrl+Z = 26
        }
        if (km->aa_down || km->one_shot || km->caps_lock) {
            c = (char)(c - 'a' + 'A');
        }
    }
    return (uint8_t)c;
}

void tab5_keymap_event(tab5_keymap_t *km, bool pressed, int row, int col, uint32_t now_ms)
{
    if (row < 0 || row >= TAB5_KEYMAP_ROWS || col < 0 || col >= TAB5_KEYMAP_COLS) {
        return;
    }
    if (at(row, col, 4, 0)) {          // Ctrl
        km->ctrl = pressed;
        return;
    }
    if (at(row, col, 3, 0)) {          // Sym
        km->sym = pressed;
        return;
    }
    if (at(row, col, 4, 1)) {          // Alt: no meaning in the radio
        return;
    }
    if (at(row, col, 3, 1)) {          // Aa
        if (pressed) {
            km->aa_down = true;
            km->aa_used = false;
            km->aa_pressed_ms = now_ms;
        } else {
            km->aa_down = false;
            if (!km->aa_used && now_ms - km->aa_pressed_ms < DOUBLE_TAP_MS) {
                // Tap: one capital; second tap soon after: caps lock; tap again: off
                if (km->caps_lock) {
                    km->caps_lock = false;
                    km->one_shot = false;
                } else if (km->one_shot && now_ms - km->aa_last_tap_ms < DOUBLE_TAP_MS) {
                    km->caps_lock = true;
                    km->one_shot = false;
                } else {
                    km->one_shot = true;
                }
                km->aa_last_tap_ms = now_ms;
            }
        }
        return;
    }

    int id = row * TAB5_KEYMAP_COLS + col;
    if (!pressed) {
        if (id == km->repeat_key) {
            km->repeat_key = -1;
        }
        return;
    }
    if (km->aa_down) {
        km->aa_used = true;
    }
    bool repeats = false;
    uint32_t value = translate(km, row, col, &repeats);
    km->one_shot = false;
    push(km, value);
    if (value && repeats) {
        km->repeat_key = id;
        km->repeat_value = value;
        km->repeat_next_ms = now_ms + REPEAT_DELAY_MS;
    } else {
        km->repeat_key = -1;
    }
}

void tab5_keymap_tick(tab5_keymap_t *km, uint32_t now_ms)
{
    if (km->repeat_key >= 0 && reached(now_ms, km->repeat_next_ms)) {
        push(km, km->repeat_value);
        km->repeat_next_ms = now_ms + REPEAT_INTERVAL_MS;   // Late ticks don't burst
    }
}
