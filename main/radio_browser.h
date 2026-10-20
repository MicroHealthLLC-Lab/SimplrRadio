// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Radio Browser API client - fetches station list from radio-browser.info

#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATION_NAME_LEN   64
#define STATION_URL_LEN    256
#define STATION_TAGS_LEN   64
#define MAX_STATIONS       60

typedef struct {
    char name[STATION_NAME_LEN];
    char url[STATION_URL_LEN];
    char tags[STATION_TAGS_LEN];
    uint32_t bitrate;
} radio_station_t;

// Fetch the stations matching the saved station search (settings.h) from the Radio Browser
// directory. Fills stations[] (names and tags as printable ASCII), returns the number fetched
// (0 on error). Blocking: call from a worker task.
int radio_browser_fetch(radio_station_t *stations, int max_stations);

#ifdef __cplusplus
}
#endif
