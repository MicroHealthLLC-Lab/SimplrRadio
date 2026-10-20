// SPDX-License-Identifier: Apache-2.0
//
// LilyGO T-Deck keyboard and trackball driver (tdeck_keyboard.c); input comes out as keys.h values.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

// On the BSP's I2C bus (opened with the touch panel); OK without a keyboard
esp_err_t tdeck_keyboard_init(i2c_master_bus_handle_t bus);
uint32_t tdeck_keyboard_read_key(void);   // 0 when nothing is waiting
bool tdeck_keyboard_present(void);
