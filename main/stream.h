// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// HTTP audio stream with ring buffer and ICY metadata parsing

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_URL_MAX 300

// ICY metadata callback - called from stream task when StreamTitle changes.
typedef void (*icy_meta_cb_t)(const char *stream_title);

// Start streaming from the given http(s) URL. Spawns an HTTP reader task. Other schemes and
// over-long URLs are refused (stream_failed() turns true).
void stream_start(const char *url, icy_meta_cb_t meta_cb);

// Stop the streaming task and drain the buffer.
void stream_stop(void);

// Read audio bytes from the ring buffer (blocks up to timeout_ms).
// Returns number of bytes read (0 on timeout / stream ended).
int stream_read(uint8_t *buf, int len, uint32_t timeout_ms);

// True if the streaming task is alive and feeding data.
bool stream_is_active(void);

// True if the last connection was refused or returned a non-2xx status
// (e.g. HTTP 403) - i.e. the station did not serve a playable stream.
bool stream_failed(void);

// Why it failed, for the screen ("HTTP 403", "no connection", "no memory"...); "" if it didn't.
const char *stream_error(void);

// True once the server answered 2xx and audio bytes are flowing.
bool stream_connected(void);

#ifdef __cplusplus
}
#endif
