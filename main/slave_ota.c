// SPDX-License-Identifier: Apache-2.0
//
// Keeps the ESP32-C6 WiFi co-processor's firmware current. Boards ship with old esp_hosted
// firmware; the matching image (slave_fw/network_adapter.bin) is embedded in this app, so it
// arrives with every install (flasher or launcher) and is covered by the app image's own
// checksum. If the C6 reports a different version, the image is checked and sent over SDIO.

#include "slave_ota.h"
#include "esp_log.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "mbedtls/sha256.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <inttypes.h>

static const char *TAG = "slave_ota";

#define CHUNK_SIZE 1500

extern const uint8_t c6_fw_start[] asm("_binary_network_adapter_bin_start");
extern const uint8_t c6_fw_end[] asm("_binary_network_adapter_bin_end");

static char s_version[24] = "unknown";

const char *slave_ota_version(void)
{
    return s_version;
}

// The image's real length (header, segments, padding, checksum, optional SHA-256) and its
// version, after checking that it is a well-formed ESP image whose appended hash matches
static bool check_image(size_t *out_len, char *version, size_t version_len)
{
    // Both symbols bound the one blob the linker embedded (target_add_binary_data)
    // cppcheck-suppress comparePointers
    size_t avail = (size_t)(c6_fw_end - c6_fw_start);
    esp_image_header_t hdr;
    if (avail < sizeof(hdr)) {
        return false;
    }
    memcpy(&hdr, c6_fw_start, sizeof(hdr));
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.segment_count == 0 || hdr.segment_count > ESP_IMAGE_MAX_SEGMENTS) {
        return false;
    }
    size_t off = sizeof(hdr);
    for (int i = 0; i < hdr.segment_count; i++) {
        esp_image_segment_header_t seg;
        if (off + sizeof(seg) > avail) {
            return false;
        }
        memcpy(&seg, c6_fw_start + off, sizeof(seg));
        if (i == 0 && seg.data_len >= sizeof(esp_app_desc_t) && off + sizeof(seg) + sizeof(esp_app_desc_t) <= avail) {
            esp_app_desc_t desc;
            memcpy(&desc, c6_fw_start + off + sizeof(seg), sizeof(desc));
            if (desc.magic_word == ESP_APP_DESC_MAGIC_WORD) {
                snprintf(version, version_len, "%.*s", (int)sizeof(desc.version), desc.version);
            }
        }
        off += sizeof(seg) + seg.data_len;
        if (off > avail) {
            return false;
        }
    }
    off = (off + 1 + 15) & ~(size_t)15;   // The checksum byte, then padding to 16 bytes

    if (hdr.hash_appended) {
        if (off + 32 > avail) {
            return false;
        }
        uint8_t digest[32];
        mbedtls_sha256(c6_fw_start, off, digest, 0);
        if (memcmp(digest, c6_fw_start + off, sizeof(digest)) != 0) {
            ESP_LOGE(TAG, "Embedded C6 image fails its SHA-256 check");
            return false;
        }
        off += 32;
    }
    *out_len = off;
    return off <= avail;
}

esp_err_t slave_ota_update_if_needed(slave_ota_progress_cb_t progress)
{
    esp_hosted_coprocessor_fwver_t ver = {0};
    bool have_ver = esp_hosted_get_coprocessor_fwversion(&ver) == ESP_OK;
    if (have_ver) {
        snprintf(s_version, sizeof(s_version), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 ver.major1, ver.minor1, ver.patch1);
        ESP_LOGI(TAG, "Co-processor firmware %s", s_version);
    } else {
        ESP_LOGW(TAG, "Co-processor version unknown (old firmware)");
    }

    size_t fw_len = 0;
    char new_ver[32] = "";
    if (!check_image(&fw_len, new_ver, sizeof(new_ver))) {
        ESP_LOGE(TAG, "Embedded C6 firmware unusable; not updating");
        return ESP_ERR_INVALID_STATE;
    }
    if (have_ver && ver.major1 != 0 && strcmp(s_version, new_ver) == 0) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Updating co-processor %s -> %s (%u bytes)", s_version, new_ver, (unsigned)fw_len);

    esp_err_t ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(ret));
        return ret;
    }
    int last_pct = -1;
    for (size_t off = 0; off < fw_len; off += CHUNK_SIZE) {
        size_t n = fw_len - off > CHUNK_SIZE ? CHUNK_SIZE : fw_len - off;
        ret = esp_hosted_slave_ota_write((uint8_t *)(c6_fw_start + off), n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed at %u", (unsigned)off);
            esp_hosted_slave_ota_end();
            return ret;
        }
        int pct = (int)((off + n) * 100 / fw_len);
        if (progress && pct != last_pct && pct % 5 == 0) {
            progress(pct);
        }
        last_pct = pct;
    }
    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (esp_hosted_slave_ota_activate() != ESP_OK) {
        ESP_LOGW(TAG, "OTA activate not supported by the old firmware; it reboots anyway");
    }

    // The C6 reboots into the new firmware; give it time, then link up again
    vTaskDelay(pdMS_TO_TICKS(5000));
    ret = esp_hosted_connect_to_slave();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reconnect after OTA failed: %s", esp_err_to_name(ret));
        return ret;
    }
    snprintf(s_version, sizeof(s_version), "%s", new_ver);
    ESP_LOGI(TAG, "Co-processor updated to %s", new_ver);
    return ESP_OK;
}
