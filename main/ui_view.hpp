/*
 * UI View
 * Everything on the screen, drawn with plain LVGL objects and a few shared styles (the
 * SimplrSSH look: dark background, one accent colour, symbol-font icons):
 *   - the title bar: back arrow, page title, volume, Bluetooth and WiFi status
 *   - menu pages (ui_page.hpp): numbered rows, details, marks, a notice card
 *   - prompt pages: a text field with an on-screen keyboard (touch) or a hardware keyboard,
 *     passwords masked with a Show toggle
 *   - Now Playing: station, song, status, a live spectrum and the control buttons
 * Sizes and fonts come from board_ui(), so it fits the Tab5's 1280x720 and the T-Deck's
 * 320x240. All calls are made with the display lock held. The on_* callbacks run in LVGL's
 * task with the lock held: they only post an event for the app task.
 */

#ifndef UI_VIEW_HPP
#define UI_VIEW_HPP

#include "lvgl.h"
#include "ui_page.hpp"
#include <functional>
#include <string>

namespace theme
{
    static const uint32_t BG = 0x0D1117;        // Screen
    static const uint32_t SURFACE = 0x161B22;   // Title bar, fields, notice card, buttons
    static const uint32_t DIVIDER = 0x21262D;   // Row separators, pressed rows
    static const uint32_t TEXT = 0xE6EDF3;
    static const uint32_t DIM = 0x8B949E;       // Details, hints
    static const uint32_t ACCENT = 0x2F81F7;    // Highlighted row, focused field
    static const uint32_t SONG = 0xF0B429;      // Song title (amber)
    static const uint32_t DANGER = 0xF85149;
    static const uint32_t OK = 0x3FB950;
    static const uint32_t WARN = 0xD29922;
}

// Now Playing's control buttons (on_button)
enum NpButton { NP_MENU, NP_PREV, NP_PAUSE, NP_NEXT, NP_MUTE, NP_VOL_DOWN, NP_VOL_UP, NP_STATIONS, NP_BUTTONS };

struct NowPlaying {
    std::string station;     // "" while there are no stations
    std::string song;
    std::string info;        // "12 / 60  -  128 kbps  -  rock, indie"
    std::string status;      // "Connecting...", "Station unavailable", "" when playing
    bool status_warn = false;
    bool muted = false;
    bool paused = false;     // The pause button shows Play
};

class UiView
{
public:
    void create(lv_obj_t* screen);

    // Title bar
    void set_title(const std::string& title);
    void set_back(bool shown);
    void set_status(bool wifi, int rssi, int volume, bool muted, bool bluetooth);   // bluetooth: a device connected

    // Menu and prompt pages; menus highlight `selected`
    void show_page(const ui::Page& page, const std::string& notice, int selected);
    void set_notice(const std::string& notice);
    void select(int row);
    int selected() const { return selected_row; }
    bool scroll(int direction);   // > 0: toward the top; false at the end

    // The prompt's text field (when the page asked a question)
    bool prompt_shown() const { return field != NULL && page_shown(); }
    void prompt_key(uint32_t key);          // Edits with a keys.h value (characters, cursor)
    std::string prompt_text() const;
    void prompt_wipe();                     // Overwrites and empties it (passwords)
    void toggle_reveal();                   // Show / hide a password
    void show_keyboard(bool shown);         // The on-screen keyboard
    bool keyboard_shown() const;

    // Now Playing
    void show_now_playing();
    bool now_playing_shown() const { return np && !lv_obj_is_hidden(np); }
    void update_now_playing(const NowPlaying& state);
    void set_list_button(const char* word);   // What the List button opens: "Stations", "Songs"

    std::function<void(int row)> on_tap;      // A row was tapped
    std::function<void()> on_back;            // Back arrow, or the keyboard's close key
    std::function<void()> on_submit;          // The on-screen keyboard's OK
    std::function<void(int button)> on_button;
    std::function<void(lv_dir_t dir)> on_swipe;

private:
    lv_obj_t* screen = NULL;
    lv_obj_t* bar = NULL;
    lv_obj_t* back_label = NULL;
    lv_obj_t* title_label = NULL;
    lv_obj_t* wifi_label = NULL;
    lv_obj_t* volume_label = NULL;
    lv_obj_t* bt_label = NULL;
    lv_obj_t* page = NULL;
    lv_obj_t* notice_label = NULL;
    lv_obj_t* field = NULL;
    lv_obj_t* reveal_button = NULL;
    lv_obj_t* keyboard = NULL;
    lv_obj_t* first_row = NULL;
    int row_count = 0;
    int selected_row = -1;

    lv_obj_t* np = NULL;
    lv_obj_t* np_info = NULL;
    lv_obj_t* np_station = NULL;
    lv_obj_t* np_song = NULL;
    lv_obj_t* np_status = NULL;
    lv_obj_t* np_spectrum = NULL;
    lv_obj_t* np_mute_label = NULL;
    lv_obj_t* np_pause_label = NULL;
    lv_obj_t* np_list_label = NULL;
    bool np_wide = false;                     // Buttons have words next to their icons
    lv_timer_t* spectrum_timer = NULL;

    bool page_shown() const { return page && !lv_obj_is_hidden(page); }
    void layout_page();
    void create_now_playing();
    lv_obj_t* row_obj(int row) const;
    void paint_row(int row, bool selected);

    static void row_clicked_cb(lv_event_t* e);
    static void back_clicked_cb(lv_event_t* e);
    static void field_clicked_cb(lv_event_t* e);
    static void keyboard_cb(lv_event_t* e);
    static void reveal_cb(lv_event_t* e);
    static void button_cb(lv_event_t* e);
    static void gesture_cb(lv_event_t* e);
    static void spectrum_draw_cb(lv_event_t* e);
    static void spectrum_timer_cb(lv_timer_t* t);
};

#endif
