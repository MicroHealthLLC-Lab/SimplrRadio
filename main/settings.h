// SPDX-License-Identifier: Apache-2.0
//
// The radio's own settings (NVS namespace "radio" in the settings partition, settings_nvs.cpp):
// station search, volume, last station. WiFi networks are wifi_mgr's; passwords the vault's.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SEARCH_COUNTRY_LEN 3     // ISO 3166 alpha-2 + NUL; "" = worldwide
#define SEARCH_STATE_LEN   48    // "" = the whole country
#define SEARCH_COUNT_MAX   60

typedef struct {
    char country[SEARCH_COUNTRY_LEN];
    char state[SEARCH_STATE_LEN];
    int count;
} station_search_t;

void settings_get_search(station_search_t *out);           // Defaults: US, Texas, 60
esp_err_t settings_set_search(const station_search_t *search);

int settings_get_volume(int fallback);
esp_err_t settings_set_volume(int volume);

bool settings_get_station(char *url, size_t len);          // Last station played
esp_err_t settings_set_station(const char *url);

#ifdef __cplusplus
}
#endif
