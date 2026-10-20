// SPDX-License-Identifier: Apache-2.0
//
// Radio settings in NVS (settings.h).

#include "settings.h"
#include "settings_nvs.hpp"
#include <cstdio>
#include <cstring>

static const char *NS = "radio";

static const char *DEFAULT_COUNTRY = "US";
static const char *DEFAULT_STATE = "Texas";
static const int DEFAULT_COUNT = SEARCH_COUNT_MAX;

static bool get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    return nvs_get_str(h, key, out, &n) == ESP_OK;
}

void settings_get_search(station_search_t *out)
{
    snprintf(out->country, sizeof(out->country), "%s", DEFAULT_COUNTRY);
    snprintf(out->state, sizeof(out->state), "%s", DEFAULT_STATE);
    out->count = DEFAULT_COUNT;

    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    // A stored empty string is a choice (worldwide / the whole country), not "unset"
    get_str(h, "country", out->country, sizeof(out->country));
    get_str(h, "state", out->state, sizeof(out->state));
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) == ESP_OK && count >= 1 && count <= SEARCH_COUNT_MAX) {
        out->count = count;
    }
    nvs_close(h);
}

esp_err_t settings_set_search(const station_search_t *search)
{
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "country", search->country);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "state", search->state);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "count", (uint8_t)search->count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

int settings_get_volume(int fallback)
{
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    uint8_t v = 0;
    int volume = nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100 ? v : fallback;
    nvs_close(h);
    return volume;
}

esp_err_t settings_set_volume(int volume)
{
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t old = 0;
    if (nvs_get_u8(h, "vol", &old) != ESP_OK || old != volume) {   // Unchanged: no flash write
        err = nvs_set_u8(h, "vol", (uint8_t)volume);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}

bool settings_get_station(char *url, size_t len)
{
    nvs_handle_t h;
    if (settings_nvs::open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    bool ok = get_str(h, "url", url, len);
    nvs_close(h);
    return ok;
}

esp_err_t settings_set_station(const char *url)
{
    nvs_handle_t h;
    esp_err_t err = settings_nvs::open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    char old[300];
    if (!get_str(h, "url", old, sizeof(old)) || strcmp(old, url) != 0) {
        err = nvs_set_str(h, "url", url);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    }
    nvs_close(h);
    return err;
}
