// SPDX-License-Identifier: Apache-2.0
//
// Player (player.h).

#include "player.h"
#include "audio.h"
#include "mp3_decoder.h"
#include "settings.h"
#include "spectrum.h"
#include "stream.h"
#include "text.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "player";

#define TUNE_DEBOUNCE_MS   300    // Paging through stations: tune once the user pauses
#define DEAD_RETRY_MS      3000   // A stream that dropped is retried after this long
#define DEAD_RETRIES       2      // ... this many times, then the next station is tuned
#define VOLUME_SAVE_MS     2000   // Flash write after the last volume change

static radio_station_t *s_stations;   // PSRAM, MAX_STATIONS
static int s_count;
static int s_index = -1;
static int s_pending = -1;            // Debounced tune, -1 = none
static TickType_t s_pending_tick;
static bool s_wanted;                 // A stream should be playing
static int s_retries;
static TickType_t s_dead_since;       // 0 = alive
static player_status_t s_status = PLAYER_IDLE;

static int s_volume = 60;
static bool s_muted;
static TickType_t s_volume_changed;   // 0 = saved

static SemaphoreHandle_t s_title_lock;
static char s_title[256];
static volatile uint32_t s_version;

static void changed(void)
{
    s_version = s_version + 1;
}

// From the stream task
static void on_title(const char *title)
{
    char clean[sizeof(s_title)];
    text_to_ascii(clean, sizeof(clean), title);
    xSemaphoreTake(s_title_lock, portMAX_DELAY);
    bool differs = strcmp(clean, s_title) != 0;
    if (differs) {
        // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - clean is char[sizeof(s_title)]
        memcpy(s_title, clean, sizeof(s_title));
    }
    xSemaphoreGive(s_title_lock);
    if (differs) {
        ESP_LOGI(TAG, "Now playing: %s", clean);
        changed();
    }
}

static void clear_title(void)
{
    xSemaphoreTake(s_title_lock, portMAX_DELAY);
    s_title[0] = '\0';
    xSemaphoreGive(s_title_lock);
}

esp_err_t player_init(void)
{
    s_title_lock = xSemaphoreCreateMutex();
    s_stations = heap_caps_calloc(MAX_STATIONS, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    if (!s_stations) {
        return ESP_ERR_NO_MEM;
    }
    spectrum_init();   // First: the decoder feeds it even if the codec failed
    esp_err_t err = audio_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Audio init failed");
        return err;
    }
    s_volume = settings_get_volume(s_volume);
    audio_set_volume(s_volume);
    return ESP_OK;
}

// Stops the stream and decoder (waits for their tasks)
static void halt(void)
{
    stream_stop();
    mp3dec_stop();
}

static void commit(int index)
{
    if (index < 0 || index >= s_count) {
        return;
    }
    halt();
    s_index = index;
    s_wanted = true;
    s_dead_since = 0;
    clear_title();
    const radio_station_t *st = &s_stations[index];
    ESP_LOGI(TAG, "Tuning [%d] %s (%u kbps)", index, st->name, (unsigned)st->bitrate);
    stream_start(st->url, on_title);
    mp3dec_start();
    s_status = stream_failed() ? PLAYER_UNAVAILABLE : PLAYER_CONNECTING;
    settings_set_station(st->url);
    changed();
}

void player_set_stations(const radio_station_t *list, int count)
{
    if (count > MAX_STATIONS) {
        count = MAX_STATIONS;
    }
    char current[STATION_URL_LEN] = "";
    if (s_index >= 0 && s_index < s_count) {
        // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - both are char[STATION_URL_LEN] (radio_station_t.url)
        memcpy(current, s_stations[s_index].url, sizeof(current));
    } else {
        settings_get_station(current, sizeof(current));
    }
    halt();
    // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - count is clamped to MAX_STATIONS above, the size s_stations was allocated with
    memcpy(s_stations, list, sizeof(radio_station_t) * count);
    s_count = count;
    s_index = -1;
    s_pending = -1;
    int start = count > 0 ? 0 : -1;
    for (int i = 0; i < count && current[0]; i++) {
        if (strcmp(s_stations[i].url, current) == 0) {
            start = i;
            break;
        }
    }
    s_status = PLAYER_IDLE;
    s_wanted = false;
    changed();
    if (start >= 0) {
        s_retries = 0;
        commit(start);
    }
}

int player_count(void) { return s_count; }
int player_index(void) { return s_index; }

const radio_station_t *player_station(int index)
{
    return index >= 0 && index < s_count ? &s_stations[index] : NULL;
}

void player_select(int index)
{
    if (index < 0 || index >= s_count) {
        return;
    }
    s_index = index;            // The screen shows the choice at once
    s_pending = index;
    s_pending_tick = xTaskGetTickCount();
    s_retries = 0;
    clear_title();
    s_status = PLAYER_CONNECTING;
    changed();
}

void player_step(int delta)
{
    if (s_count > 0) {
        int from = s_index < 0 ? 0 : s_index;
        player_select(((from + delta) % s_count + s_count) % s_count);
    }
}

void player_stop(void)
{
    s_pending = -1;
    if (s_wanted) {
        halt();
        s_wanted = false;
        s_status = PLAYER_IDLE;
        changed();
    }
}

void player_resume(void)
{
    if (s_index >= 0 && s_pending < 0) {
        s_retries = 0;
        commit(s_index);
    }
}

void player_tick(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_volume_changed && now - s_volume_changed >= pdMS_TO_TICKS(VOLUME_SAVE_MS)) {
        s_volume_changed = 0;
        settings_set_volume(s_volume);
    }

    if (s_pending >= 0) {
        if (now - s_pending_tick >= pdMS_TO_TICKS(TUNE_DEBOUNCE_MS)) {
            int index = s_pending;
            s_pending = -1;
            commit(index);
        }
        return;
    }
    if (!s_wanted) {
        return;
    }

    player_status_t status = s_status;
    if (stream_is_active()) {
        s_dead_since = 0;
        status = mp3dec_has_audio() ? PLAYER_PLAYING : PLAYER_CONNECTING;
        if (status == PLAYER_PLAYING) {
            s_retries = 0;
        }
    } else if (stream_failed()) {
        status = PLAYER_UNAVAILABLE;   // Refused: stay, the user pages elsewhere
    } else {
        // Connected, then the stream ended or dropped: retry it, then move on
        status = PLAYER_CONNECTING;
        if (s_dead_since == 0) {
            s_dead_since = now;
        } else if (now - s_dead_since >= pdMS_TO_TICKS(DEAD_RETRY_MS)) {
            if (s_retries < DEAD_RETRIES) {
                s_retries++;
                ESP_LOGW(TAG, "Stream dropped; retry %d", s_retries);
                commit(s_index);
            } else {
                ESP_LOGW(TAG, "Stream keeps dropping; next station");
                s_retries = 0;
                commit((s_index + 1) % s_count);
            }
            return;
        }
    }
    if (status != s_status) {
        s_status = status;
        changed();
    }
}

player_status_t player_status(void) { return s_status; }

void player_title(char *buf, size_t len)
{
    if (!len) {
        return;
    }
    xSemaphoreTake(s_title_lock, portMAX_DELAY);
    snprintf(buf, len, "%s", s_title);
    xSemaphoreGive(s_title_lock);
}

int player_volume(void) { return s_volume; }
bool player_muted(void) { return s_muted; }

void player_set_volume(int volume)
{
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    if (s_muted) {
        player_set_muted(false);
    }
    if (volume != s_volume) {
        s_volume = volume;
        audio_set_volume(volume);
        s_volume_changed = xTaskGetTickCount();
        if (!s_volume_changed) {
            s_volume_changed = 1;
        }
        changed();
    }
}

void player_set_muted(bool muted)
{
    if (muted != s_muted) {
        s_muted = muted;
        audio_pa_enable(!muted);
        changed();
    }
}

uint32_t player_version(void) { return s_version; }
