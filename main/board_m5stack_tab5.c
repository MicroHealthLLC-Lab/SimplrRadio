// Copyright 2025-2026 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Board: M5Stack Tab5 (ESP32-P4), every hardware revision.
//   Display : 5" 720x1280 MIPI-DSI shown as 1280x720 landscape. The espressif/m5stack_tab5 BSP
//             detects the panel (ILI9881C + GT911 touch, ST7123, or ST7121) by probing the
//             touch controller; the old fixed ST7123 bring-up left the other panels black.
//             LVGL's frame is rotated by the PPA.
//   Touch   : GT911 or ST7123/ST712x, as an LVGL pointer (from the BSP)
//   Keyboard: optional Tab5 Keyboard (tab5_keyboard.c)
//   Audio   : ES8388 codec (BSP), biquad EQ, speaker muted while headphones are plugged in
//   Power   : two PI4IOE5V6408 IO expanders: speaker amp, side-port 5 V, ESP32-C6 power, charger
//   WiFi    : the ESP32-C6 co-processor over SDIO (esp_hosted + esp_wifi_remote); Bluetooth LE
//             through it too (esp_hosted HCI, NimBLE on the P4)
//   SD card : microSD on SDMMC slot 0 (4-bit), powered by the P4's LDO channel 4; the C6 is on
//             slot 1 of the same controller

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_io_expander.h"
#include "esp_codec_dev.h"
#include "dsps_biquad.h"
#include "dsps_biquad_gen.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "esp_hosted.h"
#include "esp_hosted_bt_host_stack.h"

#include "audio.h"
#include "board.h"
#include "tab5_keyboard.h"

static const char *TAG = "board_tab5";

const char *board_name(void) { return "M5Stack Tab5"; }

// IO expander pins (M5Stack Tab5 pin map)
#define IO_RF_EXTERNAL   IO_EXPANDER_PIN_NUM_0   // 0x43 P0: antenna, low = internal
#define IO_SPEAKER_EN    IO_EXPANDER_PIN_NUM_1   // 0x43 P1: NS4150B amplifier
#define IO_EXT5V_EN      IO_EXPANDER_PIN_NUM_2   // 0x43 P2: 5 V to the side port (keyboard)
#define IO_HP_DETECT     IO_EXPANDER_PIN_NUM_7   // 0x43 P7: headphones plugged in (input)
#define IO1_WLAN_PWR_EN  IO_EXPANDER_PIN_NUM_0   // 0x44 P0: ESP32-C6 power
#define IO1_USB5V_EN     IO_EXPANDER_PIN_NUM_3   // 0x44 P3: 5 V out of the USB-A port
#define IO1_PWROFF       IO_EXPANDER_PIN_NUM_4   // 0x44 P4: power-off pulses
#define IO1_NQC_EN       IO_EXPANDER_PIN_NUM_5   // 0x44 P5: low = quick charge
#define IO1_CHARGE_EN    IO_EXPANDER_PIN_NUM_7   // 0x44 P7: IP2326 charger on

#define DEFAULT_RATE 44100

static esp_io_expander_handle_t s_io;

// -- Power ----------------------------------------------------------------
void board_init_power(void)
{
    // Both expanders are reset by their driver on first use, which leaves every pin
    // high-impedance: each output must be set push-pull, or it never drives (the cause of
    // SimplrSSH's black-screen / no-WiFi Tab5 boots). Levels as M5Stack's firmware sets them.
    s_io = bsp_io_expander_init();
    esp_io_expander_handle_t io1 = bsp_io_expander1_init();
    if (!s_io || !io1) {
        ESP_LOGE(TAG, "IO expanders not reachable");
        return;
    }
    // Internal antenna; speaker amplifier off until audio starts; side-port 5 V on (keyboard)
    const uint32_t outs = IO_RF_EXTERNAL | IO_SPEAKER_EN | IO_EXT5V_EN;
    esp_io_expander_set_dir(s_io, outs, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(s_io, IO_RF_EXTERNAL | IO_SPEAKER_EN, 0);
    esp_io_expander_set_level(s_io, IO_EXT5V_EN, 1);
    esp_io_expander_set_output_mode(s_io, outs, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);

    // Power the WiFi co-processor now, so it has booted by the time WiFi is first used. The
    // battery only charges while the firmware enables the charger (standard charging, as the
    // factory firmware does). USB-A 5 V off, no power-off pulse.
    const uint32_t outs1 = IO1_WLAN_PWR_EN | IO1_USB5V_EN | IO1_PWROFF | IO1_NQC_EN | IO1_CHARGE_EN;
    esp_io_expander_set_dir(io1, outs1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io1, IO1_USB5V_EN | IO1_PWROFF | IO1_CHARGE_EN, 0);
    esp_io_expander_set_level(io1, IO1_WLAN_PWR_EN | IO1_NQC_EN, 1);
    esp_io_expander_set_output_mode(io1, outs1, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(io1, IO1_CHARGE_EN, 1);
}

// -- Display --------------------------------------------------------------
lv_display_t *board_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            .task_stack = 8192,
            .task_affinity = -1,
            .task_max_sleep_ms = 500,
            .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
            .timer_period_ms = 5,
        },
        // Quarter-screen draw buffers in PSRAM (2 x 460 KB), never internal RAM
        .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES / 4,
        .double_buffer = true,
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .sw_rotate = true,   // Rotated by the PPA (CONFIG_LVGL_PORT_ENABLE_PPA)
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Display start failed");
        return NULL;
    }
    // The panel is 720x1280 portrait; the radio (and the keyboard) use it landscape
    if (bsp_display_lock(0)) {
        bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90);
        bsp_display_unlock();
    }
    bsp_display_brightness_set(100);
    return disp;
}

void board_display_brightness(int percent)
{
    bsp_display_brightness_set(percent);
}

const board_ui_t *board_ui(void)
{
    static const board_ui_t ui = {
        .small = &lv_font_montserrat_16,
        .text = &lv_font_montserrat_20,
        .title = &lv_font_montserrat_24,
        .list = &lv_font_montserrat_24,
        .big = &lv_font_montserrat_40,
        .bar_h = 52,
        .row_h = 68,
        .gap = 8,
        .button_h = 84,
        .keyboard_h = 300,
        .key_back = "Esc",
        .key_reveal = "Tab",
    };
    return &ui;
}

// -- WiFi co-processor ----------------------------------------------------
// As SimplrSSH's Tab5 board: esp_hosted links to the C6 over SDIO once the C6 has power
// (board_init_power), then the usual esp_wifi_* calls are forwarded to it (esp_wifi_remote).
// esp_hosted 3.x works with the firmware the C6 ships with, so it is never updated.
static bool s_wifi_ready;
static char s_c6_version[24];

esp_err_t board_wifi_prepare(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "WiFi co-processor not responding");
        return ESP_FAIL;
    }
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        snprintf(s_c6_version, sizeof(s_c6_version), "%u.%u.%u", (unsigned)ver.major1,
                 (unsigned)ver.minor1, (unsigned)ver.patch1);
    }
    s_wifi_ready = true;
    return ESP_OK;
}

const char *board_wifi_firmware(void)
{
    return s_c6_version[0] ? s_c6_version : "unknown";
}

// Bluetooth LE through the C6 as well: esp_hosted carries HCI over the same SDIO link, NimBLE
// runs here. The C6's firmware must have its Bluetooth controller (setup() waits up to 5 s).
esp_err_t board_bt_prepare(void)
{
    esp_err_t err = board_wifi_prepare();
    if (err != ESP_OK) {
        return err;
    }
    esp_hosted_bt_host_stack_cfg_t cfg = ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT();
    err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 Bluetooth controller not available: %s", esp_err_to_name(err));
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

void board_bt_release(void)
{
    esp_hosted_bt_host_stack_teardown();
}

// -- SD card --------------------------------------------------------------
// The BSP's slot settings, mounted here rather than with bsp_sdcard_mount(): that one creates
// the card's LDO each time and leaks it when there is no card, so a second try always failed.
// The controller is shared with esp_hosted (the C6 on slot 1): ESP-IDF 5.5 skips a second host
// init, and an unmount or a failed mount releases slot 0 only (SDMMC_HOST_DEFAULT's deinit_p),
// so the WiFi link is never reset. Default speed (20 MHz): plenty for audio.
static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_sd_ldo;

esp_err_t board_sd_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }
    if (!s_sd_ldo) {
        sd_pwr_ctrl_ldo_config_t ldo = {
            .ldo_chan_id = 4,
        };
        esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_sd_ldo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD card power (LDO 4) failed");
            return err;
        }
    }
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    host.pwr_ctrl_handle = s_sd_ldo;
    sdmmc_slot_config_t slot;
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    const esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,   // Never erase someone's card
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "No SD card (%s)", esp_err_to_name(err));
    }
    return err;
}

void board_sd_unmount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, s_card);
        s_card = NULL;
    }
}

bool board_sd_mounted(void) { return s_card != NULL; }
const char *board_sd_root(void) { return BSP_SD_MOUNT_POINT; }

// -- Keyboard -------------------------------------------------------------
esp_err_t board_keyboard_init(void) { return tab5_keyboard_init(); }
uint32_t board_read_key(void) { return tab5_keyboard_read_key(); }
bool board_keyboard_present(void) { return tab5_keyboard_present(); }

// -- EQ presets (biquad DSP on decoded PCM) -------------------------------
// Flat (bypass) on the speaker, Bass on headphones. Coefficients are rebuilt whenever the
// sample rate or preset changes; processing happens in audio_write() before the codec.
typedef enum { EQ_FLAT = 0, EQ_BASS, EQ_VOICE } eq_preset_t;

#define EQ_MAX_BANDS   2
#define EQ_CHUNK       256                   // Frames processed per inner loop
#define EQ_BUF_SAMPLES 2304                  // One MP3 frame, stereo

static float       s_eq_coef[EQ_MAX_BANDS][5];
static float       s_eq_wL[EQ_MAX_BANDS][2]; // Per-channel biquad delay lines
static float       s_eq_wR[EQ_MAX_BANDS][2];
static int         s_eq_bands;
static eq_preset_t s_eq_preset = EQ_FLAT;
static int16_t     s_eq_buf[EQ_BUF_SAMPLES];

static void eq_build(eq_preset_t preset, int sample_rate)
{
    float fs = (float)sample_rate;
    int bands = 0;
    // Fresh filter state (not a secret): zero every biquad delay line
    for (int b = 0; b < EQ_MAX_BANDS; b++) {
        s_eq_wL[b][0] = s_eq_wL[b][1] = 0.0f;
        s_eq_wR[b][0] = s_eq_wR[b][1] = 0.0f;
    }
    switch (preset) {
    case EQ_BASS:
        // Low shelf +4 dB at 120 Hz: warmth for headphones without clipping
        dsps_biquad_gen_lowShelf_f32(s_eq_coef[0], 120.0f / fs, 4.0f, 0.7f);
        bands = 1;
        break;
    case EQ_VOICE:
        // High-pass 120 Hz (rumble) and high shelf -3 dB at 6 kHz (sibilance)
        dsps_biquad_gen_hpf_f32(s_eq_coef[0], 120.0f / fs, 0.7f);
        dsps_biquad_gen_highShelf_f32(s_eq_coef[1], 6000.0f / fs, -3.0f, 0.7f);
        bands = 2;
        break;
    default:
        break;
    }
    s_eq_bands = bands;
    s_eq_preset = preset;
}

static void eq_apply(int16_t *buf, int n_stereo)
{
    float tmp[EQ_CHUNK];
    int frames = n_stereo / 2;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < frames; i += EQ_CHUNK) {
            int chunk = (i + EQ_CHUNK <= frames) ? EQ_CHUNK : (frames - i);
            for (int j = 0; j < chunk; j++) {
                tmp[j] = buf[(i + j) * 2 + ch] * (1.0f / 32768.0f);
            }
            for (int b = 0; b < s_eq_bands; b++) {
                dsps_biquad_f32(tmp, tmp, chunk, s_eq_coef[b], ch == 0 ? s_eq_wL[b] : s_eq_wR[b]);
            }
            for (int j = 0; j < chunk; j++) {
                float v = tmp[j] * 32768.0f;
                buf[(i + j) * 2 + ch] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)v;
            }
        }
    }
}

// -- Audio (audio.h): ES8388 through the BSP ------------------------------
// The 3.5 mm jack doesn't switch the speaker off: it only reports "inserted" on expander P7.
// The speaker plays only while the user hasn't muted AND no headphones are in.
static esp_codec_dev_handle_t s_spk;
static SemaphoreHandle_t s_route_lock;
static int  s_sample_rate = DEFAULT_RATE;
static int  s_out_vol = 60;          // Re-applied after the codec is reopened
static bool s_user_pa_on = true;
static bool s_hp_inserted;
static bool s_spk_on = true;         // bsp_audio_codec_speaker_init() turns the amp on

static void apply_spk_route(void)
{
    if (!s_spk) {
        return;   // Audio didn't start
    }
    xSemaphoreTake(s_route_lock, portMAX_DELAY);
    bool amp = s_user_pa_on && !s_hp_inserted;
    if (amp != s_spk_on) {
        // Soft-mute the DAC around the amp switch, so neither output pops
        esp_codec_dev_set_out_mute(s_spk, true);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(s_io, IO_SPEAKER_EN, amp);
        s_spk_on = amp;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Mute is the DAC's: it silences the headphones too, not just the speaker amp
    esp_codec_dev_set_out_mute(s_spk, !s_user_pa_on);
    // Bass for headphones, flat for the speaker
    eq_preset_t eq = s_hp_inserted ? EQ_BASS : EQ_FLAT;
    if (eq != s_eq_preset) {
        eq_build(eq, s_sample_rate);
    }
    xSemaphoreGive(s_route_lock);
}

static bool read_hp_inserted(void)
{
    uint32_t level = 0;
    if (esp_io_expander_get_level(s_io, IO_HP_DETECT, &level) != ESP_OK) {
        return s_hp_inserted;
    }
    return (level & IO_HP_DETECT) != 0;
}

static void hp_detect_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool now = read_hp_inserted();
        if (now != s_hp_inserted) {
            s_hp_inserted = now;
            ESP_LOGI(TAG, "Headphones %s", now ? "plugged in: speaker off" : "removed");
            apply_spk_route();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static esp_err_t codec_open(int rate)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = (uint32_t)rate,
    };
    return esp_codec_dev_open(s_spk, &fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_init(void)
{
    s_route_lock = xSemaphoreCreateMutex();   // Before any return: mute takes it
    if (!s_io || !s_route_lock) {
        return ESP_FAIL;
    }
    // Stereo I2S (the BSP's default is mono) before the codec is created on it
    i2s_std_config_t i2s_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(DEFAULT_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
        },
    };
    if (bsp_i2c_init() != ESP_OK || bsp_audio_init(&i2s_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return ESP_FAIL;
    }
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk || codec_open(DEFAULT_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 codec init failed");
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_spk, s_out_vol);
    s_sample_rate = DEFAULT_RATE;

    // Route from the jack's state now (no amp blip at boot), then follow it
    s_hp_inserted = read_hp_inserted();
    apply_spk_route();
    xTaskCreate(hp_detect_task, "hp_detect", 3072, NULL, 4, NULL);
    return ESP_OK;
}

esp_err_t audio_write(const int16_t *samples, size_t num_samples, size_t *samples_written,
                      uint32_t timeout_ms)
{
    (void)timeout_ms;
    const void *data = samples;
    if (s_eq_bands > 0 && num_samples > 0 && num_samples <= EQ_BUF_SAMPLES) {
        // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - num_samples <= EQ_BUF_SAMPLES, s_eq_buf's length, is checked above
        memcpy(s_eq_buf, samples, num_samples * sizeof(int16_t));
        eq_apply(s_eq_buf, (int)num_samples);
        data = s_eq_buf;
    }
    int ret = esp_codec_dev_write(s_spk, (void *)data, num_samples * sizeof(int16_t));
    if (samples_written) {
        *samples_written = ret == ESP_CODEC_DEV_OK ? num_samples : 0;
    }
    return ret == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_set_sample_rate(int rate)
{
    if (rate < 8000 || rate > 96000) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rate == s_sample_rate) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Sample rate %d -> %d", s_sample_rate, rate);
    esp_codec_dev_set_out_mute(s_spk, true);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_codec_dev_close(s_spk);
    esp_err_t err = codec_open(rate);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reopen at %d Hz failed; keeping %d Hz", rate, s_sample_rate);
        codec_open(s_sample_rate);
    } else {
        s_sample_rate = rate;
        xSemaphoreTake(s_route_lock, portMAX_DELAY);
        eq_build(s_eq_preset, rate);   // Coefficients depend on the sample rate
        xSemaphoreGive(s_route_lock);
    }
    esp_codec_dev_set_out_vol(s_spk, s_out_vol);   // Reopening resets the volume
    esp_codec_dev_set_out_mute(s_spk, !s_user_pa_on);
    return err;
}

esp_err_t audio_set_volume(int vol)
{
    s_out_vol = vol;
    return esp_codec_dev_set_out_vol(s_spk, vol) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_pa_enable(bool on)
{
    s_user_pa_on = on;
    apply_spk_route();
}
