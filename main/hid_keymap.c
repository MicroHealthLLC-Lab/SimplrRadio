// SPDX-License-Identifier: Apache-2.0
//
// HID usages to the radio's keys (hid_keymap.h).

#include "hid_keymap.h"
#include "keys.h"

#define MOD_CTRL   0x11   // Left or right Ctrl
#define MOD_SHIFT  0x22   // Left or right Shift

// Usages 0x1E-0x38: the digit row and punctuation, unshifted and shifted (US layout)
static const char DIGITS[] = "1234567890";
static const char DIGITS_SHIFTED[] = "!@#$%^&*()";
static const char PUNCT[] = "-=[]\\#;'`,./";          // 0x2D-0x38 (0x32 is the non-US #)
static const char PUNCT_SHIFTED[] = "_+{}|~:\"~<>?";
static const char KEYPAD[] = "/*-+\n1234567890.";     // 0x54-0x63

uint32_t hid_key_from_usage(uint8_t usage, uint8_t modifiers)
{
    int shift = (modifiers & MOD_SHIFT) != 0;
    if (usage >= 0x04 && usage <= 0x1D) {
        return (uint32_t)((shift ? 'A' : 'a') + (usage - 0x04));
    }
    if (usage >= 0x1E && usage <= 0x27) {
        return (uint32_t)(shift ? DIGITS_SHIFTED : DIGITS)[usage - 0x1E];
    }
    if (usage >= 0x2D && usage <= 0x38) {
        return (uint32_t)(shift ? PUNCT_SHIFTED : PUNCT)[usage - 0x2D];
    }
    if (usage >= 0x54 && usage <= 0x63) {
        char c = KEYPAD[usage - 0x54];
        return c == '\n' ? KEY_ENTER : (uint32_t)c;
    }
    switch (usage) {
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESC;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return ' ';
        case 0x4A: return KEY_HOME;
        case 0x4B: return KEY_PAGE_UP;
        case 0x4C: return (modifiers & MOD_CTRL) ? KEY_GO_HOME : KEY_DELETE;
        case 0x4D: return KEY_END;
        case 0x4E: return KEY_PAGE_DOWN;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        case 0x7F: return KEY_MUTE;
        case 0x80: return KEY_VOL_UP;
        case 0x81: return KEY_VOL_DOWN;
        default: return 0;
    }
}

uint32_t hid_key_from_consumer(uint16_t usage)
{
    switch (usage) {
        case 0xB0:            // Play
        case 0xB1:            // Pause
        case 0xB7:            // Stop
        case 0xCD:            // Play/Pause
            return KEY_PLAY_PAUSE;
        case 0xB5: return KEY_NEXT_TRACK;
        case 0xB6: return KEY_PREV_TRACK;
        case 0xE2: return KEY_MUTE;
        case 0xE9: return KEY_VOL_UP;
        case 0xEA: return KEY_VOL_DOWN;
        // A remote's navigation
        case 0x41: return KEY_ENTER;        // Menu pick
        case 0x42: return KEY_UP;
        case 0x43: return KEY_DOWN;
        case 0x44: return KEY_LEFT;
        case 0x45: return KEY_RIGHT;
        case 0x46: return KEY_ESC;          // Menu escape
        case 0x223: return KEY_GO_HOME;     // AC Home
        case 0x224: return KEY_ESC;         // AC Back
        default: return 0;
    }
}

static int held_key(const hid_input_t *in, uint8_t usage)
{
    for (int i = 0; in && i < in->n_keys; i++) {
        if (in->keys[i] == usage) {
            return 1;
        }
    }
    return 0;
}

static int held_consumer(const hid_input_t *in, uint16_t usage)
{
    for (int i = 0; in && i < in->n_consumer; i++) {
        if (in->consumer[i] == usage) {
            return 1;
        }
    }
    return 0;
}

size_t hid_new_keys(const hid_input_t *before, const hid_input_t *now, uint32_t *out, size_t max)
{
    size_t n = 0;
    for (int i = 0; i < now->n_keys && n < max; i++) {
        if (!held_key(before, now->keys[i])) {
            uint32_t key = hid_key_from_usage(now->keys[i], now->modifiers);
            if (key) {
                out[n++] = key;
            }
        }
    }
    for (int i = 0; i < now->n_consumer && n < max; i++) {
        if (!held_consumer(before, now->consumer[i])) {
            uint32_t key = hid_key_from_consumer(now->consumer[i]);
            if (key) {
                out[n++] = key;
            }
        }
    }
    return n;
}
