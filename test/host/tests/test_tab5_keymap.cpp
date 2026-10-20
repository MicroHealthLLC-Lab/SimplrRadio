/*
 * Tab5 Keymap (tab5_keymap.c): raw row/column presses from the keyboard become keys.
 */

#include "tab5_keymap.h"
#include "keys.h"
#include "test.hpp"

static tab5_keymap_t km;

static void press(int row, int col, uint32_t t = 0)
{
    tab5_keymap_event(&km, true, row, col, t);
    tab5_keymap_event(&km, false, row, col, t + 10);
}

static uint32_t pop()
{
    return tab5_keymap_pop(&km);
}

TEST(keymap_letters_and_digits)
{
    tab5_keymap_reset(&km);
    press(2, 1);    // q
    press(0, 1);    // 1
    press(4, 13);   // space
    CHECK_EQ(pop(), (uint32_t)'q');
    CHECK_EQ(pop(), (uint32_t)'1');
    CHECK_EQ(pop(), (uint32_t)' ');
    CHECK_EQ(pop(), (uint32_t)0);
}

TEST(keymap_special_keys)
{
    tab5_keymap_reset(&km);
    press(0, 0);    // Esc
    press(3, 13);   // Enter
    press(2, 13);   // Backspace
    press(2, 0);    // Tab
    press(0, 13);   // Del
    CHECK_EQ(pop(), (uint32_t)KEY_ESC);
    CHECK_EQ(pop(), (uint32_t)KEY_ENTER);
    CHECK_EQ(pop(), (uint32_t)KEY_BACKSPACE);
    CHECK_EQ(pop(), (uint32_t)KEY_TAB);
    CHECK_EQ(pop(), (uint32_t)KEY_DELETE);
}

TEST(keymap_arrows_and_sym_layer)
{
    tab5_keymap_reset(&km);
    press(3, 11);
    press(4, 11);
    press(4, 10);
    press(4, 12);
    CHECK_EQ(pop(), (uint32_t)KEY_UP);
    CHECK_EQ(pop(), (uint32_t)KEY_DOWN);
    CHECK_EQ(pop(), (uint32_t)KEY_LEFT);
    CHECK_EQ(pop(), (uint32_t)KEY_RIGHT);

    tab5_keymap_event(&km, true, 3, 0, 0);   // Sym held
    press(3, 11);
    press(4, 12);
    press(1, 1);    // Sym+! = ?
    tab5_keymap_event(&km, false, 3, 0, 50);
    CHECK_EQ(pop(), (uint32_t)KEY_PAGE_UP);
    CHECK_EQ(pop(), (uint32_t)KEY_END);
    CHECK_EQ(pop(), (uint32_t)'?');
}

TEST(keymap_ctrl_del_goes_home)
{
    tab5_keymap_reset(&km);
    tab5_keymap_event(&km, true, 4, 0, 0);   // Ctrl
    press(0, 13);
    press(2, 3);    // Ctrl+E = 5
    tab5_keymap_event(&km, false, 4, 0, 30);
    CHECK_EQ(pop(), (uint32_t)KEY_GO_HOME);
    CHECK_EQ(pop(), (uint32_t)5);
}

TEST(keymap_aa_shift_one_shot_and_caps_lock)
{
    tab5_keymap_reset(&km);
    // Held: shift
    tab5_keymap_event(&km, true, 3, 1, 0);
    press(3, 2, 10);   // a
    tab5_keymap_event(&km, false, 3, 1, 40);
    CHECK_EQ(pop(), (uint32_t)'A');
    // Tap: one capital
    press(3, 1, 1000);
    press(3, 2, 1100);
    press(3, 2, 1200);
    CHECK_EQ(pop(), (uint32_t)'A');
    CHECK_EQ(pop(), (uint32_t)'a');
    // Double tap: caps lock until tapped again
    press(3, 1, 3000);
    press(3, 1, 3100);
    press(3, 2, 3200);
    press(3, 2, 3300);
    CHECK_EQ(pop(), (uint32_t)'A');
    CHECK_EQ(pop(), (uint32_t)'A');
    press(3, 1, 5000);
    press(3, 2, 5100);
    CHECK_EQ(pop(), (uint32_t)'a');
}

TEST(keymap_auto_repeat)
{
    tab5_keymap_reset(&km);
    tab5_keymap_event(&km, true, 2, 13, 0);   // Backspace held
    CHECK_EQ(pop(), (uint32_t)KEY_BACKSPACE);
    tab5_keymap_tick(&km, 100);
    CHECK_EQ(pop(), (uint32_t)0);             // Not before the delay
    tab5_keymap_tick(&km, 460);
    CHECK_EQ(pop(), (uint32_t)KEY_BACKSPACE);
    tab5_keymap_tick(&km, 520);
    CHECK_EQ(pop(), (uint32_t)KEY_BACKSPACE);
    tab5_keymap_event(&km, false, 2, 13, 530);
    tab5_keymap_tick(&km, 2000);
    CHECK_EQ(pop(), (uint32_t)0);
    // Enter never repeats
    tab5_keymap_event(&km, true, 3, 13, 3000);
    tab5_keymap_tick(&km, 4000);
    CHECK_EQ(pop(), (uint32_t)KEY_ENTER);
    CHECK_EQ(pop(), (uint32_t)0);
}

TEST(keymap_out_of_range_ignored_and_queue_bounded)
{
    tab5_keymap_reset(&km);
    tab5_keymap_event(&km, true, 7, 3, 0);
    tab5_keymap_event(&km, true, 1, 14, 0);
    CHECK_EQ(pop(), (uint32_t)0);
    for (int i = 0; i < 40; i++) {
        press(2, 1, i * 20);
    }
    int n = 0;
    while (pop()) {
        n++;
    }
    CHECK_EQ(n, TAB5_KEYMAP_QUEUE);
}
