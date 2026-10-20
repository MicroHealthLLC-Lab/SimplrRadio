// SPDX-License-Identifier: Apache-2.0
//
// M5Stack Tab5 Keyboard driver (tab5_keyboard.c); keys come out as keys.h values.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t tab5_keyboard_init(void);     // Opens the keyboard's I2C bus; OK without a keyboard
uint32_t tab5_keyboard_read_key(void);  // 0 when no key is waiting
bool tab5_keyboard_present(void);
