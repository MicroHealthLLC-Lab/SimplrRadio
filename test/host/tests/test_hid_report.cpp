/*
 * HID reports (hid_report.c): report maps of real keyboards and remotes parsed, input reports
 * decoded, and malformed or oversized maps refused without reading past their end.
 */

#include "hid_report.h"
#include "test.hpp"
#include <cstring>
#include <vector>

// A keyboard with report ID 1 (boot layout: modifiers, reserved, six keys) and a consumer
// control with report ID 2 (one 16-bit usage), as many Bluetooth keyboards describe themselves
static const std::vector<uint8_t> KEYBOARD_AND_MEDIA = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,   // Generic desktop, keyboard, report 1
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,   // Modifiers
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                // Reserved byte (constant)
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,   // LEDs (output)
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,   // Keys
    0xC0,
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02,   // Consumer control, report 2
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

// A media remote without report IDs: one bit per listed usage
static const std::vector<uint8_t> REMOTE_BITS = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x06,
    0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xE9, 0x09, 0xEA, 0x09, 0xE2, 0x81, 0x02,
    0x95, 0x02, 0x81, 0x01,   // Padding
    0xC0,
};

TEST(hid_keyboard_map_parsed)
{
    hid_map_t map;
    CHECK(hid_parse_map(KEYBOARD_AND_MEDIA.data(), KEYBOARD_AND_MEDIA.size(), &map));
    CHECK(map.has_ids);
    CHECK(hid_map_uses(&map, 1));
    CHECK(hid_map_uses(&map, 2));
    CHECK(!hid_map_uses(&map, 3));
}

TEST(hid_keyboard_report_decoded)
{
    hid_map_t map;
    CHECK(hid_parse_map(KEYBOARD_AND_MEDIA.data(), KEYBOARD_AND_MEDIA.size(), &map));
    const uint8_t report[] = {0x02, 0x00, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00};   // Left Shift, a, b
    hid_input_t in;
    hid_decode(&map, 1, report, sizeof(report), &in);
    CHECK_EQ(in.modifiers, (uint8_t)0x02);
    CHECK_EQ((int)in.n_keys, 2);
    CHECK_EQ(in.keys[0], (uint8_t)0x04);
    CHECK_EQ(in.keys[1], (uint8_t)0x05);
    CHECK_EQ((int)in.n_consumer, 0);
}

TEST(hid_consumer_report_decoded)
{
    hid_map_t map;
    CHECK(hid_parse_map(KEYBOARD_AND_MEDIA.data(), KEYBOARD_AND_MEDIA.size(), &map));
    const uint8_t play_pause[] = {0xCD, 0x00};
    hid_input_t in;
    hid_decode(&map, 2, play_pause, sizeof(play_pause), &in);
    CHECK_EQ((int)in.n_consumer, 1);
    CHECK_EQ(in.consumer[0], (uint16_t)0xCD);
    CHECK_EQ((int)in.n_keys, 0);
    const uint8_t released[] = {0x00, 0x00};
    hid_decode(&map, 2, released, sizeof(released), &in);
    CHECK_EQ((int)in.n_consumer, 0);
}

TEST(hid_bitfield_remote_decoded)
{
    hid_map_t map;
    CHECK(hid_parse_map(REMOTE_BITS.data(), REMOTE_BITS.size(), &map));
    CHECK(!map.has_ids);
    const uint8_t vol_up[] = {0x08};   // Bit 3: Volume Up (0xE9)
    hid_input_t in;
    hid_decode(&map, 0, vol_up, sizeof(vol_up), &in);
    CHECK_EQ((int)in.n_consumer, 1);
    CHECK_EQ(in.consumer[0], (uint16_t)0xE9);
}

TEST(hid_short_report_reads_nothing_past_its_end)
{
    hid_map_t map;
    CHECK(hid_parse_map(KEYBOARD_AND_MEDIA.data(), KEYBOARD_AND_MEDIA.size(), &map));
    const uint8_t report[] = {0x01};   // Only the modifiers arrived
    hid_input_t in;
    hid_decode(&map, 1, report, sizeof(report), &in);
    CHECK_EQ(in.modifiers, (uint8_t)0x01);
    CHECK_EQ((int)in.n_keys, 0);
}

TEST(hid_error_rollover_is_no_key)
{
    hid_map_t map;
    CHECK(hid_parse_map(KEYBOARD_AND_MEDIA.data(), KEYBOARD_AND_MEDIA.size(), &map));
    const uint8_t report[] = {0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};   // Too many keys held
    hid_input_t in;
    hid_decode(&map, 1, report, sizeof(report), &in);
    CHECK_EQ((int)in.n_keys, 0);
}

TEST(hid_boot_keyboard_decoded)
{
    const uint8_t report[] = {0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00};   // Enter
    hid_input_t in;
    hid_decode_boot_keyboard(report, sizeof(report), &in);
    CHECK_EQ((int)in.n_keys, 1);
    CHECK_EQ(in.keys[0], (uint8_t)0x28);
    hid_decode_boot_keyboard(report, 2, &in);   // Too short
    CHECK_EQ((int)in.n_keys, 0);
}

TEST(hid_malformed_maps_refused)
{
    hid_map_t map;
    // An item whose data runs past the end
    const uint8_t truncated[] = {0x05, 0x07, 0x26, 0xFF};
    CHECK(!hid_parse_map(truncated, sizeof(truncated), &map));
    // Report ID 0 is reserved
    const uint8_t id_zero[] = {0x85, 0x00};
    CHECK(!hid_parse_map(id_zero, sizeof(id_zero), &map));
    // A report far larger than any real one
    const uint8_t huge[] = {0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x75, 0x08, 0x96, 0xFF, 0x7F, 0x81, 0x00};
    CHECK(!hid_parse_map(huge, sizeof(huge), &map));
    // A long item whose length runs past the end is skipped, not read
    const uint8_t long_item[] = {0xFE, 0x40, 0x00};
    CHECK(!hid_parse_map(long_item, sizeof(long_item), &map));
    // Nothing the radio uses (a mouse)
    const uint8_t mouse[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
                             0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0xC0};
    CHECK(!hid_parse_map(mouse, sizeof(mouse), &map));
    // Oversized and empty
    std::vector<uint8_t> big(HID_MAX_MAP_BYTES + 1, 0xC0);
    CHECK(!hid_parse_map(big.data(), big.size(), &map));
    CHECK(!hid_parse_map(nullptr, 0, &map));
}

TEST(hid_field_count_bounded)
{
    // More input fields than the map keeps: parsing stays within HID_MAX_FIELDS
    std::vector<uint8_t> desc = {0x05, 0x0C, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01};
    for (int i = 0; i < HID_MAX_FIELDS + 10; i++) {
        desc.insert(desc.end(), {0x09, 0xCD, 0x81, 0x02});
    }
    hid_map_t map;
    CHECK(hid_parse_map(desc.data(), desc.size(), &map));
    CHECK_EQ((int)map.count, HID_MAX_FIELDS);
}
