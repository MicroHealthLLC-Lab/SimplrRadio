// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board: Waveshare ESP32-P4-WIFI6-Touch-LCD-4B
//   Display : 720x720 MIPI-DSI (ST7703), RGB565, driven by LVGL (esp_lvgl_port)
//   Touch   : GT911 capacitive (direct I2C), an LVGL pointer input
//   Audio   : ES8311 codec + I2S
//   Keyboard: none (text is typed on the on-screen keyboard)

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_st7703.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "es8311.h"

#include "audio.h"
#include "board.h"

static const char *TAG = "board_wvshr";

const char *board_name(void) { return "Waveshare ESP32-P4 720x720"; }

void board_init_power(void)
{
    // Nothing switched: the panel, codec and WiFi co-processor are powered from the start
}

esp_err_t board_keyboard_init(void) { return ESP_OK; }
uint32_t board_read_key(void) { return 0; }
bool board_keyboard_present(void) { return false; }

const board_ui_t *board_ui(void)
{
    static const board_ui_t ui = {
        .small = &lv_font_montserrat_14,
        .text = &lv_font_montserrat_16,
        .title = &lv_font_montserrat_20,
        .list = &lv_font_montserrat_20,
        .big = &lv_font_montserrat_32,
        .bar_h = 44,
        .row_h = 60,
        .gap = 6,
        .button_h = 72,
        .keyboard_h = 280,
    };
    return &ui;
}

static bool touch_read(uint16_t *x, uint16_t *y);

// -------------------------------------------------------------------------
// Display - 720x720 ST7703, RGB565; LVGL draws partial buffers into the panel's framebuffer
// -------------------------------------------------------------------------
#define LCD_W             720
#define LCD_H             720
#define DSI_LANE_NUM      2
#define DSI_LANE_MBPS     480
#define DSI_DPI_CLK_MHZ   38
#define DSI_PHY_LDO_CHAN  3
#define DSI_PHY_LDO_MV    2500
#define DSI_BK_LIGHT_GPIO 26
#define DSI_RST_GPIO      27

static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_panel_io_handle_t s_dbi_io;

static void panel_init(void)
{

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = DSI_PHY_LDO_CHAN,
        .voltage_mv = DSI_PHY_LDO_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo));

    esp_lcd_dsi_bus_handle_t dsi_bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = DSI_LANE_NUM,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &s_dbi_io));

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = 1,
        .video_timing = {
            .h_size = LCD_W, .v_size = LCD_H,
            .hsync_back_porch = 50, .hsync_pulse_width = 20, .hsync_front_porch = 50,
            .vsync_back_porch = 20, .vsync_pulse_width = 4,  .vsync_front_porch = 20,
        },
        .flags = { .use_dma2d = true },
    };

    st7703_vendor_config_t vendor_cfg = {
        .flags = { .use_mipi_interface = 1 },
        .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = DSI_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7703(s_dbi_io, &dev_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    gpio_config_t bk_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DSI_BK_LIGHT_GPIO,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_cfg));
    gpio_set_level(DSI_BK_LIGHT_GPIO, 0);

}

// The GT911 as LVGL's pointer
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static esp_err_t touch_init(void);

lv_display_t *board_display_start(void)
{
    panel_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_stack = 8192;
    port_cfg.timer_period_ms = 5;
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LVGL start failed");
        return NULL;
    }
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = s_dbi_io,
        .panel_handle = panel_handle,
        .buffer_size = LCD_W * LCD_H / 4,   // Quarter-screen buffers in PSRAM
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
        },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags = { .avoid_tearing = false } };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display failed");
        return NULL;
    }

    // The GT911 shares the codec's I2C bus, which audio_init() opens; open it here first
    if (touch_init() == ESP_OK && lvgl_port_lock(0)) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, disp);
        lvgl_port_unlock();
    }
    board_display_brightness(100);
    return disp;
}

void board_display_brightness(int percent)
{
    gpio_set_level(DSI_BK_LIGHT_GPIO, percent > 0 ? 0 : 1);   // Backlight enable is active low
}

// -------------------------------------------------------------------------
// Audio - ES8311 + I2S
// -------------------------------------------------------------------------
#define I2C_SCL         GPIO_NUM_8
#define I2C_SDA         GPIO_NUM_7
#define I2S_MCK         GPIO_NUM_13
#define I2S_BCK         GPIO_NUM_12
#define I2S_WS          GPIO_NUM_10
#define I2S_DOUT        GPIO_NUM_9
#define I2S_DIN         GPIO_NUM_11
#define PA_EN           GPIO_NUM_53

#define I2C_PORT        I2C_NUM_0
#define I2C_CLK_HZ      100000
#define ES8311_ADDR     ES8311_ADDRRES_0   // 0x18

#define DEFAULT_RATE    44100
#define MCLK_MULTIPLE   256

static i2s_chan_handle_t s_tx_handle;
static bool              s_i2c_ready;

// The codec and the touch controller share this bus (legacy I2C driver, as es8311 uses)
static esp_err_t i2c_bus_init(void)
{
    if (s_i2c_ready) {
        return ESP_OK;
    }
    i2c_config_t i2c_cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_CLK_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_PORT, &i2c_cfg);
    if (err == ESP_OK) {
        err = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }
    s_i2c_ready = true;
    return ESP_OK;
}
static es8311_handle_t   s_codec;
static int               s_sample_rate = DEFAULT_RATE;

esp_err_t audio_init(void)
{
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PA_EN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_EN, 1);

    if (i2c_bus_init() != ESP_OK) {
        return ESP_FAIL;
    }

    s_codec = es8311_create(I2C_PORT, ES8311_ADDR);
    if (!s_codec) {
        ESP_LOGE(TAG, "Failed to create ES8311 handle");
        return ESP_FAIL;
    }

    es8311_clock_config_t clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = DEFAULT_RATE * MCLK_MULTIPLE,
        .sample_frequency = DEFAULT_RATE,
    };
    ESP_ERROR_CHECK(es8311_init(s_codec, &clk,
                                ES8311_RESOLUTION_16, ES8311_RESOLUTION_16));
    ESP_ERROR_CHECK(es8311_voice_volume_set(s_codec, 60, NULL));
    ESP_ERROR_CHECK(es8311_microphone_config(s_codec, false));
    ESP_LOGI(TAG, "ES8311 initialized at %d Hz", DEFAULT_RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_WS,
            .dout = I2S_DOUT,
            .din  = I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_LOGI(TAG, "I2S TX enabled at %d Hz stereo", DEFAULT_RATE);

    return ESP_OK;
}

esp_err_t audio_write(const int16_t *samples, size_t num_samples,
                      size_t *samples_written, uint32_t timeout_ms)
{
    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(s_tx_handle, samples,
                                       num_samples * sizeof(int16_t),
                                       &bytes_written, timeout_ms);
    if (samples_written) *samples_written = bytes_written / sizeof(int16_t);
    return ret;
}

esp_err_t audio_set_sample_rate(int rate)
{
    if (rate < 8000 || rate > 96000) {
        ESP_LOGW(TAG, "Ignoring invalid sample rate %d", rate);
        return ESP_ERR_INVALID_ARG;
    }
    if (rate == s_sample_rate) return ESP_OK;
    ESP_LOGI(TAG, "Changing sample rate %d -> %d", s_sample_rate, rate);

    esp_err_t err;
    if ((err = i2s_channel_disable(s_tx_handle)) != ESP_OK) goto fail;

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK, .bclk = I2S_BCK, .ws = I2S_WS,
            .dout = I2S_DOUT, .din = I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = MCLK_MULTIPLE;
    if ((err = i2s_channel_reconfig_std_clock(s_tx_handle, &std_cfg.clk_cfg)) != ESP_OK)
        goto reenable;
    if ((err = es8311_sample_frequency_config(s_codec, rate * MCLK_MULTIPLE, rate)) != ESP_OK)
        goto reenable;

    i2s_channel_enable(s_tx_handle);
    s_sample_rate = rate;
    return ESP_OK;

reenable:
    i2s_channel_enable(s_tx_handle);
fail:
    ESP_LOGW(TAG, "Sample rate change to %d failed: %s", rate, esp_err_to_name(err));
    return err;
}

esp_err_t audio_set_volume(int vol)
{
    return es8311_voice_volume_set(s_codec, vol, NULL);
}

void audio_pa_enable(bool on)
{
    gpio_set_level(PA_EN, on ? 1 : 0);
}

// -------------------------------------------------------------------------
// Touch - GT911 read for LVGL's pointer input
// Direct I2C using legacy driver; v1 doesn't handle GT911's 16-bit reg addr.
// -------------------------------------------------------------------------
#define GT911_ADDR       0x5D
#define GT911_ADDR_ALT   0x14
#define GT911_PRODUCT_ID 0x8140
#define GT911_X_RES_L    0x8048
#define GT911_READ_XY    0x814E
#define GT911_POINT1     0x814F

static uint8_t  s_gt911_addr;
static uint16_t s_gt911_x_res = 720, s_gt911_y_res = 720;
static int      s_i2c_errors;
static bool     s_last_down;            // held across no-refresh polls
static uint16_t s_last_x, s_last_y;

static esp_err_t gt911_read(uint16_t reg, uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t gt911_write_byte(uint16_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (!cmd) return ESP_ERR_NO_MEM;
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_gt911_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, (reg >> 8) & 0xFF, true);
    i2c_master_write_byte(cmd, reg & 0xFF, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(20));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t touch_init(void)
{
    if (i2c_bus_init() != ESP_OK) {
        return ESP_FAIL;
    }
    const uint8_t addrs[] = { GT911_ADDR, GT911_ADDR_ALT };
    for (int a = 0; a < 2; a++) {
        s_gt911_addr = addrs[a];
        uint8_t id[4] = {0};
        if (gt911_read(GT911_PRODUCT_ID, id, 4) == ESP_OK) {
            ESP_LOGI(TAG, "GT911 at 0x%02X, product ID: %c%c%c%c",
                     addrs[a], id[0], id[1], id[2], id[3]);
            uint8_t res[4] = {0};
            if (gt911_read(GT911_X_RES_L, res, 4) == ESP_OK) {
                s_gt911_x_res = res[0] | (res[1] << 8);
                s_gt911_y_res = res[2] | (res[3] << 8);
            }
            ESP_LOGI(TAG, "GT911 resolution: %dx%d", s_gt911_x_res, s_gt911_y_res);
            return ESP_OK;
        }
    }
    s_gt911_addr = 0;
    ESP_LOGE(TAG, "GT911 not found");
    return ESP_FAIL;
}

static bool touch_read(uint16_t *x, uint16_t *y)
{
    if (!s_gt911_addr) return false;

    // Heavy backoff after repeated I2C errors: hold last reported state.
    if (s_i2c_errors > 5) { s_i2c_errors--; goto out; }

    uint8_t status = 0;
    if (gt911_read(GT911_READ_XY, &status, 1) != ESP_OK) { s_i2c_errors++; goto out; }
    s_i2c_errors = 0;

    uint8_t num_points = status & 0x0F;
    bool buffer_ready  = (status & 0x80) != 0;
    if (buffer_ready) gt911_write_byte(GT911_READ_XY, 0);

    // No refresh: keep the previously reported state (matches old driver).
    if (!buffer_ready) goto out;

    if (num_points > 0) {
        uint8_t point[8] = {0};
        if (gt911_read(GT911_POINT1, point, 8) == ESP_OK) {
            uint16_t raw_x = point[1] | (point[2] << 8);
            uint16_t raw_y = point[3] | (point[4] << 8);
            uint16_t px, py;
            if (s_gt911_x_res > 0 && s_gt911_y_res > 0 &&
                (s_gt911_x_res != LCD_W || s_gt911_y_res != LCD_H)) {
                px = (uint16_t)((uint32_t)raw_x * LCD_W / s_gt911_x_res);
                py = (uint16_t)((uint32_t)raw_y * LCD_H / s_gt911_y_res);
            } else {
                px = raw_x; py = raw_y;
            }
            if (px >= LCD_W) px = LCD_W - 1;
            if (py >= LCD_H) py = LCD_H - 1;
            s_last_x = px; s_last_y = py;
            s_last_down = true;
        }
    } else {
        s_last_down = false;
    }

out:
    *x = s_last_x;
    *y = s_last_y;
    return s_last_down;
}
