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

// Bluetooth (bt_hid.cpp), before NimBLE starts (worker task, blocking). Tab5: links NimBLE to
// the ESP32-C6's Bluetooth controller over esp_hosted (after board_wifi_prepare()); fails when
// the C6's firmware has no Bluetooth. T-Deck: nothing, the controller is in the ESP32-S3.
// board_bt_release() after NimBLE stopped.
esp_err_t board_bt_prepare(void);
void board_bt_release(void);

// The microSD card (FAT32, long file names), for the music library. Blocking: worker task.
// ESP_OK once it is mounted at board_sd_root() (or already was); an error without a card or
// with one that isn't FAT. Tab5: SDMMC slot 0 (the C6 keeps slot 1). T-Deck: SPI, on the
// display's bus (chip select GPIO 39).
esp_err_t board_sd_mount(void);
void board_sd_unmount(void);
bool board_sd_mounted(void);
const char *board_sd_root(void);   // "/sdcard"

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
