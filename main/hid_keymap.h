// SPDX-License-Identifier: Apache-2.0
//
// Bluetooth keyboard and remote keys (hid_report.h input) to the radio's keys (keys.h): US
// layout characters with Shift, Enter / Esc / Backspace / Tab, arrows and paging keys,
// Ctrl+Delete for Now Playing (as on the Tab5 Keyboard), and the media keys of keyboards and
// remotes (Play/Pause, next / previous, volume, mute, the remote's arrows, select and back).
// No ESP-IDF headers (host tests).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "hid_report.h"

#ifdef __cplusplus
extern "C" {
#endif

// A keyboard usage (page 0x07) with the modifiers held; 0 if the radio has no use for it
uint32_t hid_key_from_usage(uint8_t usage, uint8_t modifiers);

// A consumer-control usage (page 0x0C); 0 if the radio has no use for it
uint32_t hid_key_from_consumer(uint16_t usage);

// The keys pressed in `now` that weren't held in `before`, in report order: at most `max`
// written to `out`, the count returned. Held keys don't repeat.
size_t hid_new_keys(const hid_input_t *before, const hid_input_t *now, uint32_t *out, size_t max);

#ifdef __cplusplus
}
#endif
