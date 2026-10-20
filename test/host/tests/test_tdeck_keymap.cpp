/*
 * T-Deck Keymap (tdeck_keymap.c): keyboard bytes and the trackball's press become keys.
 */

#include "tdeck_keymap.h"
#include "keys.h"
#include "test.hpp"

TEST(tdeck_bytes_printable_pass_through)
{
    CHECK_EQ(tdeck_key_from_byte('a'), (uint32_t)'a');
    CHECK_EQ(tdeck_key_from_byte('Z'), (uint32_t)'Z');
    CHECK_EQ(tdeck_key_from_byte('7'), (uint32_t)'7');
    CHECK_EQ(tdeck_key_from_byte(' '), (uint32_t)' ');
    CHECK_EQ(tdeck_key_from_byte('~'), (uint32_t)'~');
}

TEST(tdeck_bytes_special_keys)
{
    CHECK_EQ(tdeck_key_from_byte('\r'), (uint32_t)KEY_ENTER);
    CHECK_EQ(tdeck_key_from_byte('\n'), (uint32_t)KEY_ENTER);
    CHECK_EQ(tdeck_key_from_byte(0x08), (uint32_t)KEY_BACKSPACE);
    CHECK_EQ(tdeck_key_from_byte(0x7F), (uint32_t)KEY_BACKSPACE);
    CHECK_EQ(tdeck_key_from_byte(0x1B), (uint32_t)KEY_ESC);
    CHECK_EQ(tdeck_key_from_byte('\t'), (uint32_t)KEY_TAB);
}

TEST(tdeck_bytes_without_meaning_are_dropped)
{
    CHECK_EQ(tdeck_key_from_byte(0), (uint32_t)0);       // No key waiting
    CHECK_EQ(tdeck_key_from_byte(0x01), (uint32_t)0);
    CHECK_EQ(tdeck_key_from_byte(0x80), (uint32_t)0);
    CHECK_EQ(tdeck_key_from_byte(0xFF), (uint32_t)0);
}

TEST(tdeck_short_press_is_enter_on_release)
{
    tdeck_press_t p = {};
    CHECK_EQ(tdeck_press_poll(&p, false, 0), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, true, 100), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, true, 300), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, false, 350), (uint32_t)KEY_ENTER);
    CHECK_EQ(tdeck_press_poll(&p, false, 400), (uint32_t)0);
}

TEST(tdeck_held_press_is_esc_once)
{
    tdeck_press_t p = {};
    CHECK_EQ(tdeck_press_poll(&p, true, 1000), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, true, 1000 + TDECK_HOLD_MS), (uint32_t)KEY_ESC);
    CHECK_EQ(tdeck_press_poll(&p, true, 1000 + TDECK_HOLD_MS * 3), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, false, 1000 + TDECK_HOLD_MS * 4), (uint32_t)0);   // No Enter after it
    // The next press starts over
    CHECK_EQ(tdeck_press_poll(&p, true, 5000), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, false, 5100), (uint32_t)KEY_ENTER);
}

TEST(tdeck_press_survives_millisecond_wrap)
{
    tdeck_press_t p = {};
    uint32_t t = 0xFFFFFF00u;
    CHECK_EQ(tdeck_press_poll(&p, true, t), (uint32_t)0);
    CHECK_EQ(tdeck_press_poll(&p, true, t + TDECK_HOLD_MS), (uint32_t)KEY_ESC);
}
