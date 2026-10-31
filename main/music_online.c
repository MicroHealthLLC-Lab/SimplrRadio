// SPDX-License-Identifier: Apache-2.0
//
// MusicBrainz genre lookup (music_online.h). Web service v2:
//   https://musicbrainz.org/doc/MusicBrainz_API (search, lookups with inc=genres)
//   https://musicbrainz.org/doc/MusicBrainz_API/Rate_Limiting (one request a second, and a
//   User-Agent that names the application and where to reach its makers)
// Replies are bounded (REPLY_MAX in PSRAM) and their text goes through text_to_ascii().

#include "music_online.h"
#include "media_tags.h"
#include "text.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "music_online";

#define REPLY_MAX       (32 * 1024)   // A reply (about 3 KB for one artist)
#define MIN_SCORE       90            // Search matches below this are other artists
#define REQUEST_GAP_MS  1100          // Between the two requests of a lookup

typedef struct {
    char *buf;
    int len;
} reply_t;

static esp_err_t on_event(esp_http_client_event_t *evt)
{
    reply_t *r = (reply_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r && evt->data_len > 0) {
        if (r->len + evt->data_len < REPLY_MAX - 1) {   // Room for it and the NUL
            const char *data = (const char *)evt->data;
            for (int i = 0; i < evt->data_len; i++) {
                r->buf[r->len + i] = data[i];
            }
            r->len += evt->data_len;
        } else {
            r->len = REPLY_MAX;   // Too big: not read
        }
    }
    return ESP_OK;
}

// GET `url`; the HTTP status (0 without an answer), the body in r
static int get(const char *url, reply_t *r)
{
    char agent[96];
    snprintf(agent, sizeof(agent), "SimplrRadio/%s ( https://github.com/MicroHealthLLC-Lab/SimplrRadio )",
             esp_app_get_description()->version);
    r->len = 0;
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = on_event,
        .user_data = r,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = agent,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return 0;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    if (r->len >= REPLY_MAX) {
        return 0;
    }
    r->buf[r->len] = '\0';
    return status;
}

// Percent-encoding of a search term inside "..." (Lucene: quotes and backslashes escaped)
static void encode(char *dst, size_t len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < len; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            dst[o++] = '%';
            dst[o++] = '5';
            dst[o++] = 'C';   // A backslash before it
        }
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

// The name of the entry with the highest "count" in a tags / genres array
static const char *top_name(const cJSON *list)
{
    const char *best = NULL;
    double best_count = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, list) {
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *count = cJSON_GetObjectItem(item, "count");
        if (cJSON_IsString(name) && name->valuestring && cJSON_IsNumber(count) && count->valuedouble > best_count) {
            best = name->valuestring;
            best_count = count->valuedouble;
        }
    }
    return best;
}

static bool mbid(const char *s)
{
    if (!s || strnlen(s, 37) != 36) {
        return false;
    }
    for (int i = 0; i < 36; i++) {
        char c = s[i];
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

music_online_result_t music_online_artist(const char *artist, char *genre, size_t genre_len, char *name,
                                          size_t name_len)
{
    genre[0] = '\0';
    name[0] = '\0';
    if (!artist || !artist[0]) {
        return MUSIC_ONLINE_NONE;
    }
    reply_t r = {.buf = heap_caps_malloc(REPLY_MAX, MALLOC_CAP_SPIRAM)};
    if (!r.buf) {
        return MUSIC_ONLINE_ERROR;
    }
    char term[3 * MEDIA_NAME_LEN + 16];
    encode(term, sizeof(term), artist);
    char url[sizeof(term) + 128];
    // The search: the best match for the name, with its free-form tags
    snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/artist/?limit=1&fmt=json&query=artist:%%22%s%%22", term);

    music_online_result_t result = MUSIC_ONLINE_ERROR;
    char id[40] = "";
    char tag[MEDIA_GENRE_LEN * 2] = "";
    int status = get(url, &r);
    if (status == 503) {
        result = MUSIC_ONLINE_BUSY;
    } else if (status == 200) {
        cJSON *root = cJSON_Parse(r.buf);
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "artists"), 0);
        const cJSON *score = cJSON_GetObjectItem(first, "score");
        const cJSON *found = cJSON_GetObjectItem(first, "name");
        const cJSON *mb_id = cJSON_GetObjectItem(first, "id");
        result = MUSIC_ONLINE_NONE;
        if (cJSON_IsNumber(score) && score->valuedouble >= MIN_SCORE && cJSON_IsString(mb_id) &&
            mbid(mb_id->valuestring)) {
            snprintf(id, sizeof(id), "%s", mb_id->valuestring);
            const char *t = top_name(cJSON_GetObjectItem(first, "tags"));   // Free-form: a fallback
            text_to_ascii(tag, sizeof(tag), t ? t : "");
            if (cJSON_IsString(found) && found->valuestring) {
                char spelled[MEDIA_NAME_LEN];
                text_to_ascii(spelled, sizeof(spelled), found->valuestring);
                if (strcasecmp(spelled, artist) == 0 && strcmp(spelled, artist) != 0) {
                    snprintf(name, name_len, "%s", spelled);
                }
            }
        }
        cJSON_Delete(root);
    }
    if (id[0]) {
        // The curated genres of the artist (the search only has free-form tags)
        vTaskDelay(pdMS_TO_TICKS(REQUEST_GAP_MS));
        snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/artist/%s?inc=genres&fmt=json", id);
        status = get(url, &r);
        char best[MEDIA_GENRE_LEN * 2] = "";
        if (status == 200) {
            cJSON *root = cJSON_Parse(r.buf);
            const char *g = top_name(cJSON_GetObjectItem(root, "genres"));
            text_to_ascii(best, sizeof(best), g ? g : "");
            cJSON_Delete(root);
        } else if (status == 503) {
            result = MUSIC_ONLINE_BUSY;
        }
        media_genre_clean(genre, genre_len, best[0] ? best : tag);
        if (genre[0]) {
            result = MUSIC_ONLINE_FOUND;
        }
    }
    heap_caps_free(r.buf);
    ESP_LOGI(TAG, "Lookup: %d", (int)result);
    return result;
}
