// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Radio Browser API client: the station search from the on-screen settings (settings.h),
// tried on several of the directory's public servers in turn.
// Docs: https://api.radio-browser.info/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "radio_browser.h"
#include "settings.h"
#include "text.h"
#include <strings.h>

#define TAG "radio_browser"

// The directory's public servers (each has its own TLS certificate); one being down or
// renamed doesn't strand the radio.
static const char *const SERVERS[] = {
    "de1.api.radio-browser.info",
    "de2.api.radio-browser.info",
    "fi1.api.radio-browser.info",
    "at1.api.radio-browser.info",
};

// Fixed part of the query: top MP3 stations by vote count, working only.
#define API_QUERY "/json/stations/search?order=votes&reverse=true&hidebroken=true&codec=MP3"

#define RESP_BUF_SIZE (128 * 1024)  // 128 KB in PSRAM - Radio Browser returns ~70 KB for 60 stations

// Percent-encode `src` into `dst` (RFC 3986 unreserved set passes through).
static void url_encode(char *dst, size_t dlen, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 3 < dlen; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

// The search URL on `server`, from the saved station search
static void build_api_url(char *url, size_t ulen, const char *server, int max_stations)
{
    station_search_t search;
    settings_get_search(&search);
    int limit = search.count;
    if (limit < 1) limit = 1;
    if (limit > max_stations) limit = max_stations;

    int n = snprintf(url, ulen, "https://%s%s&limit=%d", server, API_QUERY, limit);
    if (search.country[0] && n > 0 && n < (int)ulen) {
        char enc[3 * SEARCH_COUNTRY_LEN];
        url_encode(enc, sizeof(enc), search.country);
        n += snprintf(url + n, ulen - n, "&countrycode=%s", enc);
    }
    if (search.state[0] && n > 0 && n < (int)ulen) {
        char enc[3 * SEARCH_STATE_LEN];
        url_encode(enc, sizeof(enc), search.state);
        snprintf(url + n, ulen - n, "&state=%s", enc);
    }
}

static bool web_url(const char *url)
{
    return strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0;
}

static char *s_resp_buf;
static int   s_resp_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (evt->data_len > 0 && s_resp_len + evt->data_len < RESP_BUF_SIZE - 1) {
            // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - the condition above keeps it inside RESP_BUF_SIZE with room for the NUL
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "Response buffer full, truncating");
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// One request to one server; the body ends up in s_resp_buf
static bool query(const char *server, int max_stations)
{
    char api_url[512];
    build_api_url(api_url, sizeof(api_url), server, max_stations);
    ESP_LOGI(TAG, "Query: %s", api_url);

    s_resp_len = 0;
    esp_http_client_config_t config = {
        .url = api_url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "SimplrRadio/2",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return false;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "%s: err=%s status=%d", server, esp_err_to_name(err), status);
        return false;
    }
    return true;
}

int radio_browser_fetch(radio_station_t *stations, int max_stations)
{
    s_resp_buf = heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_resp_buf) {
        ESP_LOGE(TAG, "No memory for response buffer");
        return 0;
    }
    bool ok = false;
    for (size_t i = 0; i < sizeof(SERVERS) / sizeof(SERVERS[0]) && !ok; i++) {
        ok = query(SERVERS[i], max_stations);
    }
    if (!ok) {
        free(s_resp_buf);
        s_resp_buf = NULL;
        return 0;
    }

    s_resp_buf[s_resp_len] = '\0';
    ESP_LOGI(TAG, "Received %d bytes from Radio Browser API", s_resp_len);

    // Parse JSON array
    cJSON *root = cJSON_Parse(s_resp_buf);
    free(s_resp_buf);
    s_resp_buf = NULL;

    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    int total = cJSON_GetArraySize(root);
    for (int i = 0; i < total && count < max_stations; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);

        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *url  = cJSON_GetObjectItem(item, "url_resolved");
        cJSON *tags = cJSON_GetObjectItem(item, "tags");
        cJSON *br   = cJSON_GetObjectItem(item, "bitrate");

        // Skip stations without a usable stream URL (http/https only, and it must fit)
        if (!cJSON_IsString(url) || !url->valuestring || !web_url(url->valuestring) ||
            strnlen(url->valuestring, STATION_URL_LEN) >= STATION_URL_LEN)
            continue;
        if (!cJSON_IsString(name))
            continue;

        // Skip talk/news/politics stations
        const char *n = name->valuestring;
        const char *t = (cJSON_IsString(tags) && tags->valuestring) ? tags->valuestring : "";
        if (strcasestr(n, "infowars") || strcasestr(n, "NOAA") ||
            strcasestr(t, "news") || strcasestr(t, "politics") ||
            strcasestr(t, "weather"))
            continue;

        radio_station_t *s = &stations[count];
        // Names and tags are shown on screen: printable ASCII only (text.h)
        text_to_ascii(s->name, sizeof(s->name), n);
        if (!s->name[0]) snprintf(s->name, sizeof(s->name), "Station %d", count + 1);
        snprintf(s->url, sizeof(s->url), "%s", url->valuestring);
        text_to_ascii(s->tags, sizeof(s->tags), t);
        double kbps = cJSON_IsNumber(br) ? br->valuedouble : 0;
        s->bitrate = kbps > 0 && kbps < 10000 ? (uint32_t)kbps : 0;

        ESP_LOGI(TAG, "[%2d] %s (%u kbps)", count, s->name, s->bitrate);
        count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Fetched %d stations", count);
    return count;
}
