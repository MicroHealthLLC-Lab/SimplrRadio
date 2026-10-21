// SPDX-License-Identifier: Apache-2.0
//
// HID reports of Bluetooth keyboards and remotes (bt_hid.cpp): the device's report descriptor
// ("report map") is parsed into the input fields the radio understands (keyboard keys and
// modifiers, consumer-control usages such as Play/Pause and Volume), then each input report is
// decoded with it. Reports arrive without their report ID (Bluetooth sends the ID in a
// descriptor of the characteristic), so field offsets are counted per report ID without the ID
// byte. Untrusted input: every length and count is bounded. No ESP-IDF headers (host tests).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HID_MAX_FIELDS     24    // Input fields kept from one report map
#define HID_MAX_USAGES     16    // Usages listed for one variable field
#define HID_MAX_KEYS       16    // Keys held at once (6 for boot keyboards)
#define HID_MAX_CONSUMER   4     // Consumer usages held at once
#define HID_MAX_MAP_BYTES  512   // Largest report map (the HID over GATT limit)
#define HID_MAX_REPORT     64    // Longest input report decoded

#define HID_PAGE_KEYBOARD  0x07
#define HID_PAGE_CONSUMER  0x0C

typedef struct {
    uint8_t report_id;       // 0: the map has no report IDs
    uint16_t usage_page;     // HID_PAGE_KEYBOARD or HID_PAGE_CONSUMER (others are not kept)
    uint16_t bit_offset;     // In the report, without the report ID byte
    uint8_t bit_size;        // 1-16
    uint8_t count;           // Elements
    bool variable;           // One bit (or value) per usage; else an array of usage indexes
    int32_t logical_min;
    uint16_t usage_min;      // Range of usages (usage_max == 0: none)
    uint16_t usage_max;
    uint16_t usages[HID_MAX_USAGES];   // Usages listed one by one
    uint8_t n_usages;
} hid_field_t;

typedef struct {
    hid_field_t fields[HID_MAX_FIELDS];
    uint8_t count;
    bool has_ids;
} hid_map_t;

// What is held down in one input report
typedef struct {
    uint8_t modifiers;                    // Bit 0 left Ctrl .. bit 7 right GUI
    uint8_t keys[HID_MAX_KEYS];           // Keyboard usages (0x04 and up)
    uint8_t n_keys;
    uint16_t consumer[HID_MAX_CONSUMER];  // Consumer-control usages
    uint8_t n_consumer;
} hid_input_t;

// Parses a report map; false if it is malformed or has nothing the radio uses
bool hid_parse_map(const uint8_t *desc, size_t len, hid_map_t *map);

// True if the map has fields for this report ID (0 when the map has no IDs)
bool hid_map_uses(const hid_map_t *map, uint8_t report_id);

// Decodes one input report of `report_id`
void hid_decode(const hid_map_t *map, uint8_t report_id, const uint8_t *data, size_t len, hid_input_t *out);

// Decodes a boot-protocol keyboard report (modifiers, reserved, six key usages)
void hid_decode_boot_keyboard(const uint8_t *data, size_t len, hid_input_t *out);

#ifdef __cplusplus
}
#endif
