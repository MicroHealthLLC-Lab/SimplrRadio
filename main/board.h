// SPDX-License-Identifier: Apache-2.0
//
// Board hardware abstraction. Each supported board provides one board_<name>.c implementing
// this file and audio.h; everything else (UI, WiFi, player) is shared. The build picks the
// board from the BOARD environment variable (CMakeLists.txt).

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

const char *board_name(void);

// First thing at boot: power rails and pin levels (Tab5: IO expanders, WiFi co-processor power;
// T-Deck: peripheral power, trackball inputs).
void board_init_power(void);

// Before the WiFi driver starts (worker task, blocking). Tab5: starts esp_hosted and links to
// the ESP32-C6 co-processor. T-Deck: nothing, WiFi is built in.
esp_err_t board_wifi_prepare(void);
// The WiFi co-processor's firmware version for the About page, NULL when there is none
const char *board_wifi_firmware(void);

// Display, touch (an LVGL pointer input) and backlight; LVGL is running when it returns.
// All LVGL calls from other tasks must hold the display lock (lvgl_port_lock).
lv_display_t *board_display_start(void);
void board_display_brightness(int percent);

// Text sizes and spacing for the screen's size and pixel density
typedef struct {
    const lv_font_t *small;   // Hints, details, on-screen keyboard
    const lv_font_t *text;    // Info text, notices, song title on small screens
    const lv_font_t *title;   // Title bar, questions, song title
    const lv_font_t *list;    // Menu rows
    const lv_font_t *big;     // Station name on the Now Playing screen
    int32_t bar_h;            // Title bar height
    int32_t row_h;            // Menu row height (a comfortable touch target)
    int32_t gap;              // Small padding
    int32_t button_h;         // Now Playing control buttons
    int32_t keyboard_h;       // On-screen keyboard
    const char *key_back;     // The key that goes back, for hints ("Esc")
    const char *key_reveal;   // The key that shows a password, for hints; NULL: tap Show
} board_ui_t;
const board_ui_t *board_ui(void);

// Physical keyboard (the Tab5 Keyboard when attached; the T-Deck's own keyboard and trackball).
// board_read_key() returns 0 when no key is waiting, else a value from keys.h. Called from the
// app task only.
esp_err_t board_keyboard_init(void);
uint32_t board_read_key(void);
bool board_keyboard_present(void);

#ifdef __cplusplus
}
#endif
