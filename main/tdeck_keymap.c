// SPDX-License-Identifier: Apache-2.0
//
// LilyGO T-Deck key map (tdeck_keymap.h). The keyboard's firmware sends one byte per key press:
// printable ASCII (Sym and Alt layers included), CR for Enter, BS for the backspace key.

#include "tdeck_keymap.h"
#include "keys.h"

uint32_t tdeck_key_from_byte(uint8_t byte)
{
    switch (byte) {
        case '\r':
        case '\n': return KEY_ENTER;
        case 0x08:
        case 0x7F: return KEY_BACKSPACE;
        case 0x1B: return KEY_ESC;
        case '\t': return KEY_TAB;
        default: return key_is_char(byte) ? byte : 0;
    }
}

uint32_t tdeck_press_poll(tdeck_press_t *press, bool pressed, uint32_t now_ms)
{
    if (pressed && !press->down) {
        press->down = true;
        press->held = false;
        press->down_ms = now_ms;
        return 0;
    }
    if (pressed && !press->held && now_ms - press->down_ms >= TDECK_HOLD_MS) {
        press->held = true;   // Once per press, while it is still held
        return KEY_ESC;
    }
    if (!pressed && press->down) {
        press->down = false;
        return press->held ? 0 : KEY_ENTER;
    }
    return 0;
}
