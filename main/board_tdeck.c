// SPDX-License-Identifier: Apache-2.0
//
// Board: LilyGO T-Deck / T-Deck Plus (ESP32-S3), after SimplrSSH's T-Deck board
// (github.com/MicroHealthLLC/SimplrSSH, MIT).
//   Power   : GPIO 10 powers the peripherals (display, keyboard, touch, amplifier, SD, LoRa)
//   Display : 2.8" ST7789 320x240 on SPI (esp_bsp_generic, set up from sdkconfig.defaults.tdeck)
//   Touch   : GT911 on the system I2C bus (SDA 18, SCL 8), an LVGL pointer
//   Input   : the ESP32-C3 keyboard and the trackball (tdeck_keyboard.c)
//   Audio   : MAX98357A I2S amplifier and speaker (BCK 7, WS 5, DOUT 6): no codec, so volume
//             and mute are applied to the samples, and the one speaker gets left + right
//   WiFi    : the ESP32-S3's own

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"

#include "audio.h"
#include "board.h"
#include "tdeck_keyboard.h"

static const char *TAG = "board_tdeck";

const char *board_name(void) { return "LilyGO T-Deck"; }

#define PIN_POWER_ON    GPIO_NUM_10
#define PIN_SD_CS       GPIO_NUM_39
#define PIN_LORA_CS     GPIO_NUM_9
#define PIN_TFT_CS      GPIO_NUM_12
#define PIN_SPI_MISO    GPIO_NUM_38
#define PIN_TOUCH_INT   GPIO_NUM_16
#define PIN_I2S_BCK     GPIO_NUM_7
#define PIN_I2S_WS      GPIO_NUM_5
#define PIN_I2S_DOUT    GPIO_NUM_6
#define TOUCH_I2C_HZ    100000

#define DEFAULT_RATE 44100

// -- Power ----------------------------------------------------------------
void board_init_power(void)
{
    // The level is set before each pin becomes an output, so the peripheral power and the SPI
    // chip selects (display, SD card, LoRa radio share the bus) never glitch low
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_num_t outs[] = {PIN_POWER_ON, PIN_SD_CS, PIN_LORA_CS, PIN_TFT_CS};
    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
        gpio_set_level(outs[i], 1);
        out.pin_bit_mask = 1ULL << outs[i];
        gpio_config(&out);
    }
    // Let the peripheral supply settle (the keyboard's ESP32-C3 boots from it)
    vTaskDelay(pdMS_TO_TICKS(250));

    // MISO pulled up: an unused SD card must not float the shared bus
    gpio_reset_pin(PIN_SPI_MISO);
    gpio_set_direction(PIN_SPI_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_SPI_MISO, GPIO_PULLUP_ONLY);

    // Trackball directions and press (GPIO 0): inputs with pull-ups
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0) | (1ULL << GPIO_NUM_1) | (1ULL << GPIO_NUM_2) |
                        (1ULL << GPIO_NUM_3) | (1ULL << GPIO_NUM_15),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&in);
}

esp_err_t board_wifi_prepare(void)
{
    return ESP_OK;   // WiFi is built into the ESP32-S3
}

const char *board_wifi_firmware(void)
{
    return NULL;
}

// -- Display and touch ----------------------------------------------------
static esp_err_t touch_new(esp_lcd_touch_handle_t *touch)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = PIN_TOUCH_INT,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
            .mirror_y = 0,
        },
    };
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &io_cfg, &io), TAG, "Touch I/O failed");
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, touch);
}

lv_display_t *board_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 8192,
            .task_affinity = -1,
            .task_max_sleep_ms = 500,
            .timer_period_ms = 5,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Display start failed");
        return NULL;
    }
    bsp_display_backlight_on();

    esp_lcd_touch_handle_t touch = NULL;
    if (touch_new(&touch) == ESP_OK) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch,
        };
        lvgl_port_add_touch(&touch_cfg);
    } else {
        ESP_LOGE(TAG, "Touch panel not found");
    }
    return disp;
}

void board_display_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

const board_ui_t *board_ui(void)
{
    static const board_ui_t ui = {
        .small = &lv_font_montserrat_12,
        .text = &lv_font_montserrat_12,
        .title = &lv_font_montserrat_14,
        .list = &lv_font_montserrat_14,
        .big = &lv_font_montserrat_20,
        .bar_h = 24,
        .row_h = 28,
        .gap = 3,
        .button_h = 30,
        .keyboard_h = 120,
        .key_back = "Hold ball",
        .key_reveal = NULL,   // No Tab key: tap Show
    };
    return &ui;
}

// -- Keyboard and trackball -----------------------------------------------
// On the I2C bus the touch panel opened (board_display_start)
esp_err_t board_keyboard_init(void) { return tdeck_keyboard_init(bsp_i2c_get_handle()); }
uint32_t board_read_key(void) { return tdeck_keyboard_read_key(); }
bool board_keyboard_present(void) { return tdeck_keyboard_present(); }

// -- Audio (audio.h): I2S to the MAX98357A ---------------------------------
#define MIX_SAMPLES 2304   // One MP3 frame, stereo

static i2s_chan_handle_t s_tx;
static int s_sample_rate = DEFAULT_RATE;
static volatile int32_t s_gain = 0;      // Q15 sample scale, from the volume
static volatile bool s_muted;
static int16_t s_mix[MIX_SAMPLES];

esp_err_t audio_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // Silence, not the last samples, when the stream stalls
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, NULL), TAG, "I2S channel failed");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_BCK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_enable(s_tx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Speaker init failed");
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }
    s_sample_rate = DEFAULT_RATE;
    return ESP_OK;
}

esp_err_t audio_write(const int16_t *samples, size_t num_samples, size_t *samples_written,
                      uint32_t timeout_ms)
{
    if (samples_written) {
        *samples_written = 0;
    }
    if (!s_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    int32_t gain = s_muted ? 0 : s_gain;
    size_t done = 0;
    while (done < num_samples) {
        size_t n = num_samples - done < MIX_SAMPLES ? num_samples - done : MIX_SAMPLES;
        n &= ~(size_t)1;   // Whole stereo frames
        if (n == 0) {
            break;
        }
        // One speaker: both channels carry (left + right) / 2, scaled by the volume
        for (size_t i = 0; i < n; i += 2) {
            int32_t mono = ((int32_t)samples[done + i] + samples[done + i + 1]) / 2;
            int16_t v = (int16_t)((mono * gain) >> 15);
            s_mix[i] = v;
            s_mix[i + 1] = v;
        }
        size_t bytes = 0;
        esp_err_t err = i2s_channel_write(s_tx, s_mix, n * sizeof(int16_t), &bytes, timeout_ms);
        done += bytes / sizeof(int16_t);
        if (err != ESP_OK) {
            break;
        }
    }
    if (samples_written) {
        *samples_written = done;
    }
    return done == num_samples ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_set_sample_rate(int rate)
{
    if (rate < 8000 || rate > 96000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx || rate == s_sample_rate) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Sample rate %d -> %d", s_sample_rate, rate);
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)rate);
    i2s_channel_disable(s_tx);
    esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (err == ESP_OK) {
        s_sample_rate = rate;
    } else {
        ESP_LOGW(TAG, "Rate %d Hz failed; keeping %d Hz", rate, s_sample_rate);
    }
    i2s_channel_enable(s_tx);
    return err;
}

esp_err_t audio_set_volume(int vol)
{
    vol = vol < 0 ? 0 : vol > 100 ? 100 : vol;
    // Loudness follows the square of the volume (closer to how the ear hears steps)
    s_gain = (int32_t)(32767LL * vol * vol / 10000);
    return ESP_OK;
}

void audio_pa_enable(bool on)
{
    s_muted = !on;
}
