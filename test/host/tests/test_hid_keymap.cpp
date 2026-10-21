/*
 * Bluetooth keys (hid_keymap.c): HID usages become the radio's keys; only new presses count.
 */

#include "hid_keymap.h"
#include "keys.h"
#include "test.hpp"
#include <cstring>

TEST(hid_letters_digits_and_shift)
{
    CHECK_EQ(hid_key_from_usage(0x04, 0), (uint32_t)'a');
    CHECK_EQ(hid_key_from_usage(0x1D, 0), (uint32_t)'z');
    CHECK_EQ(hid_key_from_usage(0x04, 0x02), (uint32_t)'A');   // Left Shift
    CHECK_EQ(hid_key_from_usage(0x04, 0x20), (uint32_t)'A');   // Right Shift
    CHECK_EQ(hid_key_from_usage(0x1E, 0), (uint32_t)'1');
    CHECK_EQ(hid_key_from_usage(0x27, 0), (uint32_t)'0');
    CHECK_EQ(hid_key_from_usage(0x1F, 0x02), (uint32_t)'@');
    CHECK_EQ(hid_key_from_usage(0x2D, 0), (uint32_t)'-');
    CHECK_EQ(hid_key_from_usage(0x34, 0x02), (uint32_t)'"');
    CHECK_EQ(hid_key_from_usage(0x38, 0x02), (uint32_t)'?');
    CHECK_EQ(hid_key_from_usage(0x2C, 0), (uint32_t)' ');
    CHECK_EQ(hid_key_from_usage(0x59, 0), (uint32_t)'1');      // Keypad 1
    CHECK_EQ(hid_key_from_usage(0x58, 0), (uint32_t)KEY_ENTER);   // Keypad Enter
}

TEST(hid_control_keys)
{
    CHECK_EQ(hid_key_from_usage(0x28, 0), (uint32_t)KEY_ENTER);
    CHECK_EQ(hid_key_from_usage(0x29, 0), (uint32_t)KEY_ESC);
    CHECK_EQ(hid_key_from_usage(0x2A, 0), (uint32_t)KEY_BACKSPACE);
    CHECK_EQ(hid_key_from_usage(0x2B, 0), (uint32_t)KEY_TAB);
    CHECK_EQ(hid_key_from_usage(0x52, 0), (uint32_t)KEY_UP);
    CHECK_EQ(hid_key_from_usage(0x51, 0), (uint32_t)KEY_DOWN);
    CHECK_EQ(hid_key_from_usage(0x50, 0), (uint32_t)KEY_LEFT);
    CHECK_EQ(hid_key_from_usage(0x4F, 0), (uint32_t)KEY_RIGHT);
    CHECK_EQ(hid_key_from_usage(0x4C, 0), (uint32_t)KEY_DELETE);
    CHECK_EQ(hid_key_from_usage(0x4C, 0x01), (uint32_t)KEY_GO_HOME);   // Ctrl+Delete, as the Tab5 Keyboard
    CHECK_EQ(hid_key_from_usage(0x80, 0), (uint32_t)KEY_VOL_UP);
    CHECK_EQ(hid_key_from_usage(0x3A, 0), (uint32_t)0);   // F1: no use
}

TEST(hid_consumer_keys)
{
    CHECK_EQ(hid_key_from_consumer(0xCD), (uint32_t)KEY_PLAY_PAUSE);
    CHECK_EQ(hid_key_from_consumer(0xB0), (uint32_t)KEY_PLAY_PAUSE);
    CHECK_EQ(hid_key_from_consumer(0xB5), (uint32_t)KEY_NEXT_TRACK);
    CHECK_EQ(hid_key_from_consumer(0xB6), (uint32_t)KEY_PREV_TRACK);
    CHECK_EQ(hid_key_from_consumer(0xE9), (uint32_t)KEY_VOL_UP);
    CHECK_EQ(hid_key_from_consumer(0xEA), (uint32_t)KEY_VOL_DOWN);
    CHECK_EQ(hid_key_from_consumer(0xE2), (uint32_t)KEY_MUTE);
    CHECK_EQ(hid_key_from_consumer(0x224), (uint32_t)KEY_ESC);
    CHECK_EQ(hid_key_from_consumer(0x41), (uint32_t)KEY_ENTER);
    CHECK_EQ(hid_key_from_consumer(0x183), (uint32_t)0);
}

TEST(hid_only_new_presses_become_keys)
{
    hid_input_t before;
    memset(&before, 0, sizeof(before));
    hid_input_t now = before;
    now.keys[0] = 0x04;   // a
    now.n_keys = 1;
    uint32_t out[8];
    CHECK_EQ(hid_new_keys(&before, &now, out, 8), (size_t)1);
    CHECK_EQ(out[0], (uint32_t)'a');
    // Still held, and b added: only b
    hid_input_t next = now;
    next.keys[1] = 0x05;
    next.n_keys = 2;
    CHECK_EQ(hid_new_keys(&now, &next, out, 8), (size_t)1);
    CHECK_EQ(out[0], (uint32_t)'b');
    // Media key pressed while letters are held
    hid_input_t media = next;
    media.consumer[0] = 0xCD;
    media.n_consumer = 1;
    CHECK_EQ(hid_new_keys(&next, &media, out, 8), (size_t)1);
    CHECK_EQ(out[0], (uint32_t)KEY_PLAY_PAUSE);
    // Output bounded
    CHECK_EQ(hid_new_keys(&before, &next, out, 1), (size_t)1);
}
