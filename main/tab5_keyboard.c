// SPDX-License-Identifier: Apache-2.0
//
// M5Stack Tab5 Keyboard (A164): an STM32 on its own I2C bus (SDA G0, SCL G1, address 0x6D)
// with an interrupt line (G50, low while key events are queued). Used in raw mode (key press /
// release by row and column); tab5_keymap.c does the rest. Polled from the app task, no task
// of its own. The keyboard may be attached or removed at any time. Ported from SimplrSSH.
//
// Register protocol (M5Tab5-Keyboard-Internal-FW): reading 0x20 pops one raw event (bit 7
// pressed, bits 6-4 row, bits 3-0 column; 0xFF when empty) as soon as the register address
// arrives, so event reads are never retried. Writing 0 to 0x01 releases the interrupt line.

#include "tab5_keyboard.h"
#include "tab5_keymap.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tab5_kbd";

#define KB_SDA          GPIO_NUM_0
#define KB_SCL          GPIO_NUM_1
#define KB_INT          GPIO_NUM_50
#define KB_ADDR         0x6D
#define KB_I2C_HZ       400000
#define I2C_TIMEOUT_MS  20

#define REG_INT_STA     0x01
#define REG_EVENT_NUM   0x02
#define REG_MODE        0x10
#define REG_KEY_EVENT   0x20
#define REG_VERSION     0xFE
#define MODE_RAW        0
#define MAX_EVENTS      32      // The keyboard's queue depth

#define POLL_MS         100     // Also read without an interrupt, in case one is missed
#define PROBE_MS        2000    // Look for a keyboard attached later
#define MAX_FAILURES    3       // Consecutive I2C errors before it counts as removed

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_failures;
static uint32_t s_next_poll_ms;
static uint32_t s_next_probe_ms;
static tab5_keymap_t s_keymap;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS) == ESP_OK;
}

static bool read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS) == ESP_OK;
}

// A keyboard was found: raw mode, empty queue, interrupt line released
static bool configure(void)
{
    uint8_t version = 0;
    if (!read_reg(REG_VERSION, &version) || !write_reg(REG_MODE, MODE_RAW) ||
        !write_reg(REG_EVENT_NUM, 0) || !write_reg(REG_INT_STA, 0)) {
        return false;
    }
    ESP_LOGI(TAG, "Keyboard attached (firmware %u)", (unsigned)version);
    return true;
}

static void lost(void)
{
    if (s_present) {
        ESP_LOGW(TAG, "Keyboard removed");
    }
    s_present = false;
    s_failures = 0;
    tab5_keymap_reset(&s_keymap);
}

esp_err_t tab5_keyboard_init(void)
{
    tab5_keymap_reset(&s_keymap);
    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << KB_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);

    // The keyboard has its own bus (the system bus on GPIO 31/32 belongs to the BSP)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,   // Any free controller
        .sda_io_num = KB_SDA,
        .scl_io_num = KB_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KB_ADDR,
        .scl_speed_hz = KB_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C device failed: %s", esp_err_to_name(err));
        return err;
    }
    // Attached or not, keep going: read_key() notices a keyboard attached later
    s_present = i2c_master_probe(s_bus, KB_ADDR, I2C_TIMEOUT_MS) == ESP_OK && configure();
    if (!s_present) {
        ESP_LOGI(TAG, "No keyboard attached");
    }
    return ESP_OK;
}

bool tab5_keyboard_present(void)
{
    return s_present;
}

// Reads every queued event into the keymap, then releases the interrupt line
static void drain(void)
{
    uint8_t n = 0;
    if (!read_reg(REG_EVENT_NUM, &n)) {
        if (++s_failures >= MAX_FAILURES) {
            lost();
        }
        return;
    }
    s_failures = 0;
    uint32_t now = now_ms();
    for (uint8_t i = 0; i < n && i < MAX_EVENTS; i++) {
        uint8_t event = 0xFF;
        if (!read_reg(REG_KEY_EVENT, &event) || event == 0xFF) {
            break;   // Not retried: the read already took the event off the keyboard's queue
        }
        tab5_keymap_event(&s_keymap, event & 0x80, (event >> 4) & 0x07, event & 0x0F, now);
    }
    write_reg(REG_INT_STA, 0);
}

uint32_t tab5_keyboard_read_key(void)
{
    if (!s_dev) {
        return 0;
    }
    uint32_t now = now_ms();
    if (!s_present) {
        if ((int32_t)(now - s_next_probe_ms) < 0) {
            return tab5_keymap_pop(&s_keymap);
        }
        s_next_probe_ms = now + PROBE_MS;
        if (i2c_master_probe(s_bus, KB_ADDR, I2C_TIMEOUT_MS) != ESP_OK || !configure()) {
            return tab5_keymap_pop(&s_keymap);
        }
        s_present = true;
    }

    uint32_t key = tab5_keymap_pop(&s_keymap);
    if (key) {
        return key;
    }
    if (gpio_get_level(KB_INT) == 0 || (int32_t)(now - s_next_poll_ms) >= 0) {
        s_next_poll_ms = now + POLL_MS;
        drain();
    }
    tab5_keymap_tick(&s_keymap, now);
    return tab5_keymap_pop(&s_keymap);
}
