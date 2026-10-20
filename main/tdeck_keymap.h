// SPDX-License-Identifier: Apache-2.0
//
// LilyGO T-Deck input as keys.h values: the bytes its keyboard (an ESP32-C3 at I2C 0x55) sends,
// and the trackball's press (Enter on release; held, Esc = back). No ESP-IDF headers: tested on
// the host (test/host/tests/test_tdeck_keymap.cpp).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A held trackball press means Esc after this long
#define TDECK_HOLD_MS 600

// A keyboard byte as a keys.h value; 0 for nothing (no key waiting, or a byte without a meaning)
uint32_t tdeck_key_from_byte(uint8_t byte);

// The trackball's press: fed its state on every poll, returns a key or 0
typedef struct {
    bool down;
    bool held;             // KEY_ESC already sent for this press
    uint32_t down_ms;
} tdeck_press_t;

uint32_t tdeck_press_poll(tdeck_press_t *press, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
