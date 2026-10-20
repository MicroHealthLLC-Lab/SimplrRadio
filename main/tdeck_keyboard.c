// SPDX-License-Identifier: Apache-2.0
//
// LilyGO T-Deck input, after SimplrSSH's T-Deck board (github.com/MicroHealthLLC/SimplrSSH, MIT):
//   Keyboard : an ESP32-C3 at I2C 0x55 on the system bus (SDA 18, SCL 8, shared with the touch
//              panel); reading one byte returns the next key, 0 when none. It boots with the
//              peripheral power, so one that doesn't answer yet is looked for again later.
//   Trackball: four direction lines (GPIO 3 up, 15 down, 1 left, 2 right) that pulse low as it
//              rolls, counted by interrupts; its press is the BOOT line (GPIO 0, low = pressed).
// Polled from the app task, no task of its own; tdeck_keymap.c turns bytes and presses into keys.

#include "tdeck_keyboard.h"
#include "tdeck_keymap.h"
#include "keys.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tdeck_kbd";

#define KB_ADDR         0x55
#define KB_I2C_HZ       100000
#define I2C_TIMEOUT_MS  20
#define PROBE_MS        2000    // Look again for a keyboard that didn't answer
#define MAX_FAILURES    3       // Consecutive I2C errors before it counts as gone

#define BALL_UP         GPIO_NUM_3
#define BALL_DOWN       GPIO_NUM_15
#define BALL_LEFT       GPIO_NUM_1
#define BALL_RIGHT      GPIO_NUM_2
#define BALL_PRESS      GPIO_NUM_0

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_failures;
static uint32_t s_next_probe_ms;
static tdeck_press_t s_press;

// Trackball pulses since the last read, per direction (counted in the interrupt handler)
static const gpio_num_t BALL_PINS[4] = {BALL_UP, BALL_DOWN, BALL_LEFT, BALL_RIGHT};
static const uint32_t BALL_KEYS[4] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
static volatile uint32_t s_pulses[4];
#define MAX_PULSES 8   // A fast roll is capped, so the cursor doesn't run away

static void IRAM_ATTR ball_isr(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    if (s_pulses[i] < MAX_PULSES) {
        s_pulses[i]++;
    }
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool probe(void)
{
    return i2c_master_probe(s_bus, KB_ADDR, I2C_TIMEOUT_MS) == ESP_OK;
}

esp_err_t tdeck_keyboard_init(i2c_master_bus_handle_t bus)
{
    // Trackball: inputs with pull-ups (board_init_power), falling edges counted
    gpio_install_isr_service(0);
    for (uint32_t i = 0; i < 4; i++) {
        gpio_set_intr_type(BALL_PINS[i], GPIO_INTR_NEGEDGE);
        gpio_isr_handler_add(BALL_PINS[i], ball_isr, (void *)(uintptr_t)i);
        gpio_intr_enable(BALL_PINS[i]);
    }

    s_bus = bus;
    if (!s_bus) {
        ESP_LOGE(TAG, "No I2C bus for the keyboard");
        return ESP_FAIL;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = KB_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C device failed: %s", esp_err_to_name(err));
        return err;
    }
    s_present = probe();
    if (!s_present) {
        ESP_LOGW(TAG, "Keyboard not answering yet");
    }
    return ESP_OK;
}

bool tdeck_keyboard_present(void)
{
    return s_present;
}

static uint32_t read_keyboard(void)
{
    uint32_t now = now_ms();
    if (!s_present) {
        if ((int32_t)(now - s_next_probe_ms) < 0) {
            return 0;
        }
        s_next_probe_ms = now + PROBE_MS;
        if (!probe()) {
            return 0;
        }
        s_present = true;
        s_failures = 0;
    }
    uint8_t byte = 0;
    if (i2c_master_receive(s_dev, &byte, 1, I2C_TIMEOUT_MS) != ESP_OK) {
        if (++s_failures >= MAX_FAILURES) {
            ESP_LOGW(TAG, "Keyboard stopped answering");
            s_present = false;
        }
        return 0;
    }
    s_failures = 0;
    return tdeck_key_from_byte(byte);
}

uint32_t tdeck_keyboard_read_key(void)
{
    // The trackball first: one key per counted pulse
    for (int i = 0; i < 4; i++) {
        if (s_pulses[i] > 0) {
            gpio_intr_disable(BALL_PINS[i]);
            s_pulses[i]--;
            gpio_intr_enable(BALL_PINS[i]);
            return BALL_KEYS[i];
        }
    }
    uint32_t key = tdeck_press_poll(&s_press, gpio_get_level(BALL_PRESS) == 0, now_ms());
    if (key) {
        return key;
    }
    return s_dev ? read_keyboard() : 0;
}
