// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// HTTP audio stream with Icecast ICY metadata support.
// Writes raw audio bytes into a FreeRTOS stream buffer; the MP3 decode
// task reads from the other side.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include "stream.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "task_stack.h"

#define TAG "stream"

#define RING_BUF_SIZE   (64 * 1024)
#define READ_BUF_SIZE   2048
#define TASK_STACK      (16 * 1024)

// -- Ring buffer ------------------------------------------------------
// One reader (the decoder). Writes and resets are serialized by s_ring_lock, and only the
// current stream's task writes: an old task that is slow to notice a stop (stuck in a
// connect or TLS handshake for up to its timeout) can never feed the next stream.
static StreamBufferHandle_t s_ringbuf;
static SemaphoreHandle_t s_ring_lock;

// -- Task control -----------------------------------------------------
// Each stream task has its own context; s_gen names the current one. A task whose generation
// is no longer current exits without touching the shared state below.
#define ICY_MAX_METAINT  (64 * 1024)   // Larger values aren't plausible; ignore the header
#define ICY_META_MAX     (255 * 16)    // Largest block the length byte can announce

typedef struct {
    uint32_t gen;
    char url[STREAM_URL_MAX];
    icy_meta_cb_t meta_cb;
    // ICY metadata: with "Icy-MetaData: 1" the server interleaves a metadata block after
    // every `metaint` audio bytes: one length byte (x16), then that many bytes of
    // "StreamTitle='...';". A block may span HTTP reads, so it is collected across reads.
    int icy_metaint;   // 0 = no ICY metadata in this stream
    int icy_count;     // Audio bytes until the next metadata block
    int meta_need;     // Bytes of the current block still expected, -1 = not in a block
    int meta_have;
    char meta_buf[ICY_META_MAX + 1];
} stream_ctx_t;

static volatile uint32_t s_gen;
static volatile bool s_running;     // The current stream's task is alive
static volatile bool s_failed;      // Connection refused / non-2xx (e.g. 403)
static char s_error[32];            // Why, for the screen (stream_error)
static volatile bool s_connected;   // Got a 2xx and is reading audio
static volatile bool s_task_done = true;

static bool current(const stream_ctx_t *ctx)
{
    return ctx->gen == s_gen;
}

static void icy_reset(stream_ctx_t *ctx, int metaint)
{
    ctx->icy_metaint = metaint;
    ctx->icy_count = metaint;
    ctx->meta_need = -1;
    ctx->meta_have = 0;
}

// Parse "StreamTitle='...'" from an ICY metadata block.
static void parse_icy_meta(stream_ctx_t *ctx, const char *meta, int len)
{
    static const char KEY[] = "StreamTitle='";
    const char *p = strnstr(meta, KEY, len);
    if (!p) return;
    p += sizeof(KEY) - 1;
    // The ICY value is delimited by "';" (e.g. StreamTitle='...';StreamUrl=...).
    // Match that pair so apostrophes inside the title (e.g. "Ain't") survive.
    const char *avail = meta + len;
    const char *end = NULL;
    for (const char *q = p; q < avail - 1; q++) {
        if (q[0] == '\'' && q[1] == ';') { end = q; break; }
    }
    // Fallback: no "';" found - use the last quote before the block ends.
    if (!end) {
        for (const char *q = avail - 1; q >= p; q--) {
            if (*q == '\'') { end = q; break; }
        }
    }
    if (!end || end <= p) return;

    int tlen = end - p;
    char title[256];
    if (tlen >= (int)sizeof(title)) tlen = sizeof(title) - 1;
    // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - tlen is clamped to sizeof(title) - 1 on the line above
    memcpy(title, p, tlen);
    title[tlen] = '\0';

    // Some stations append a station URL to the title
    // (e.g. "Artist - Song http://example.com"). Chop it off.
    char *url = strstr(title, "http://");
    if (!url) url = strstr(title, "https://");
    if (url) {
        // Trim trailing separators/whitespace left before the URL.
        while (url > title && (url[-1] == ' ' || url[-1] == '-' ||
                               url[-1] == '|' || url[-1] == '\t')) {
            url--;
        }
        *url = '\0';
    }

    if (ctx->meta_cb && current(ctx)) ctx->meta_cb(title);
}

// Feed raw bytes into the ring buffer, taking out the ICY metadata blocks.
// `data` is raw HTTP body bytes (interleaved audio + ICY blocks when enabled).
static void feed_data(stream_ctx_t *ctx, const uint8_t *data, int len)
{
    while (len > 0 && current(ctx)) {
        if (ctx->icy_metaint > 0 && ctx->icy_count == 0) {
            if (ctx->meta_need < 0) {
                // The block's length byte
                ctx->meta_need = *data * 16;
                ctx->meta_have = 0;
                data++; len--;
            }
            int take = ctx->meta_need - ctx->meta_have;
            if (take > len) take = len;
            // noopengrep: mh.gitlab.c_buffer_rule-memcpy-CopyMemory - meta_have + take <= meta_need <= 255 * 16 = ICY_META_MAX, and meta_buf is ICY_META_MAX + 1
            memcpy(ctx->meta_buf + ctx->meta_have, data, take);
            ctx->meta_have += take;
            data += take; len -= take;
            if (ctx->meta_have < ctx->meta_need) {
                continue;   // The rest arrives with the next read
            }
            if (ctx->meta_need > 0) {
                ctx->meta_buf[ctx->meta_have] = '\0';
                parse_icy_meta(ctx, ctx->meta_buf, ctx->meta_have);
            }
            ctx->meta_need = -1;
            ctx->icy_count = ctx->icy_metaint;
            continue;
        }

        // Audio bytes up to the next metadata boundary
        int chunk = len;
        if (ctx->icy_metaint > 0 && chunk > ctx->icy_count)
            chunk = ctx->icy_count;

        // Short blocking window so a stop is noticed quickly; only the current stream writes
        size_t written = 0;
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        if (current(ctx)) {
            written = xStreamBufferSend(s_ringbuf, data, chunk, pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(s_ring_lock);
        data += written;
        len  -= written;
        if (ctx->icy_metaint > 0) ctx->icy_count -= written;

        if (written == 0 && current(ctx)) {
            // Buffer full and timed out (decoder stalled): drop the rest of this read. Keep
            // the metadata position in step, or every later block would be read as audio.
            int drop = len;
            if (ctx->icy_metaint > 0 && drop > ctx->icy_count) drop = ctx->icy_count;
            data += drop;
            len -= drop;
            if (ctx->icy_metaint > 0) ctx->icy_count -= drop;
        }
    }
}

// -- HTTP event handler -----------------------------------------------
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    stream_ctx_t *ctx = (stream_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_HEADER && ctx && evt->header_key && evt->header_value) {
        if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
            char *end = NULL;
            long v = strtol(evt->header_value, &end, 10);
            icy_reset(ctx, end != evt->header_value && v > 0 && v <= ICY_MAX_METAINT ? (int)v : 0);
        }
    }
    return ESP_OK;
}

// -- Streaming task ---------------------------------------------------
// The task's end: shared state is only the current stream's to change
static void finish(stream_ctx_t *ctx, const char *error)
{
    if (current(ctx)) {
        if (error) {
            snprintf(s_error, sizeof(s_error), "%s", error);
            s_failed = true;
        }
        s_connected = false;
        s_running = false;
        s_task_done = true;
    }
    ESP_LOGI(TAG, "Stream task exiting");
    heap_caps_free(ctx);
    task_stack_exit();
}

static void stream_task(void *arg)
{
    stream_ctx_t *ctx = (stream_ctx_t *)arg;
    ESP_LOGI(TAG, "Connecting to %s", ctx->url);

    esp_http_client_config_t cfg = {
        .url            = ctx->url,
        .event_handler  = http_event,
        .user_data      = ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms     = 10000,
        .buffer_size    = READ_BUF_SIZE,
        .user_agent     = "VLC/3.0.20 LibVLC/3.0.20",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        finish(ctx, "no memory");
        return;
    }

    // Request ICY metadata
    esp_http_client_set_header(client, "Icy-MetaData", "1");

    // Open with redirect handling (esp_http_client_open doesn't auto-follow)
    const char *error = NULL;
    char http_error[16];
    uint8_t *buf = NULL;
    int max_redirects = 5;
    int status = 0;
    while (max_redirects-- > 0 && current(ctx)) {
        icy_reset(ctx, 0);
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            error = err == ESP_ERR_NO_MEM ? "no memory" : "no connection";
            goto done;
        }
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d", status);
        if (status == 301 || status == 302 || status == 307 || status == 308) {
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                ESP_LOGE(TAG, "Redirect failed");
                error = "bad redirect";
                goto done;
            }
            // Re-set ICY header (lost after redirect)
            esp_http_client_set_header(client, "Icy-MetaData", "1");
            continue;
        }
        break;
    }
    if (!current(ctx)) {
        goto done;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP error %d", status);
        snprintf(http_error, sizeof(http_error), "HTTP %d", status);
        error = http_error;
        goto done;
    }

    buf = malloc(READ_BUF_SIZE);
    if (!buf) {
        error = "no memory";
        goto done;
    }
    s_connected = true;
    while (current(ctx)) {
        int n = esp_http_client_read(client, (char *)buf, READ_BUF_SIZE);
        if (n < 0) {
            ESP_LOGE(TAG, "Read error");
            break;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "Stream ended");
            break;
        }
        feed_data(ctx, buf, n);
    }

done:
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    finish(ctx, error);
}

// -- Public API -------------------------------------------------------

void stream_start(const char *url, icy_meta_cb_t meta_cb)
{
    stream_stop();
    s_failed = false;
    s_error[0] = '\0';
    // Station URLs come from a public directory: only plain web streams are opened
    if (!url || (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) ||
        strnlen(url, STREAM_URL_MAX) >= STREAM_URL_MAX) {
        ESP_LOGW(TAG, "Refusing stream URL");
        snprintf(s_error, sizeof(s_error), "%s", "bad link");
        s_failed = true;
        return;
    }
    if (!s_ringbuf) {
        s_ring_lock = xSemaphoreCreateMutex();
#if CONFIG_IDF_TARGET_ESP32S3
        // FreeRTOS objects come from internal RAM, and the S3 has no 64 KB of it to spare once
        // WiFi and the display run: the buffer goes to PSRAM (only tasks touch it)
        s_ringbuf = xStreamBufferCreateWithCaps(RING_BUF_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_ringbuf = xStreamBufferCreate(RING_BUF_SIZE, 1);
#endif
    }
    stream_ctx_t *ctx = heap_caps_calloc(1, sizeof(stream_ctx_t), MALLOC_CAP_SPIRAM);
    if (!ctx || !s_ringbuf || !s_ring_lock) {
        heap_caps_free(ctx);
        snprintf(s_error, sizeof(s_error), "%s", "no memory");
        s_failed = true;
        return;
    }
    snprintf(ctx->url, sizeof(ctx->url), "%s", url);
    ctx->meta_cb = meta_cb;
    ctx->gen = s_gen;
    icy_reset(ctx, 0);

    s_running   = true;
    s_task_done = false;   // Before the task exists: a stop right after start waits for it
    if (task_stack_start(stream_task, "stream", TASK_STACK, ctx, 5) != pdPASS) {
        ESP_LOGE(TAG, "No memory for the stream task");
        snprintf(s_error, sizeof(s_error), "%s", "no memory");
        heap_caps_free(ctx);
        s_running = false;
        s_task_done = true;
        s_failed = true;
    }
}

void stream_stop(void)
{
    s_gen = s_gen + 1;   // The running task (if any) is no longer current
    // Give it a moment to leave (frees its socket); one stuck in a connect exits on its own
    // later without touching the next stream
    for (int i = 0; i < 25 && !s_task_done; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    s_running = false;
    s_connected = false;
    s_task_done = true;
    if (s_ringbuf) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);   // No writer is inside a send now
        xStreamBufferReset(s_ringbuf);
        xSemaphoreGive(s_ring_lock);
    }
}

int stream_read(uint8_t *buf, int len, uint32_t timeout_ms)
{
    if (!s_ringbuf) return 0;
    return xStreamBufferReceive(s_ringbuf, buf, len, pdMS_TO_TICKS(timeout_ms));
}

bool stream_is_active(void)
{
    return s_running;
}

bool stream_failed(void)
{
    return s_failed;
}

const char *stream_error(void)
{
    return s_error;
}

bool stream_connected(void)
{
    return s_connected && s_running;
}
