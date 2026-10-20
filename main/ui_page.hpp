/*
 * UI Page
 * What a menu or prompt screen shows, as plain data: a title, status lines, then either rows
 * the user picks (by number, trackball / arrow keys or touch) or a question answered on the
 * input line. Picking a row answers with its key, exactly as typing it would, so the menus'
 * input handling doesn't depend on how the answer was given. ui_view.cpp draws pages.
 * Shared with SimplrSSH (github.com/MicroHealthLLC/SimplrSSH, MIT).
 * No ESP-IDF or LVGL headers, so it is tested on the host (test/host).
 */

#ifndef UI_PAGE_HPP
#define UI_PAGE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui
{
    // Drawn at the right of a row
    enum class Mark : uint8_t {
        None,
        More,         // Opens another menu (chevron)
        On, Off,      // A setting that is on or off
        Checked, Unchecked,   // Checkbox
        Chosen, NotChosen,    // One of several choices (radio)
    };

    enum class Tone : uint8_t {
        Normal,
        Danger,       // Erases or forgets something (red)
        Back,         // Leaves the menu (dim)
    };

    struct Row {
        std::string key;              // The answer it stands for ("1", "y", "t")
        std::string label;
        std::string detail;           // Current value or status, shown dimmer
        const char* icon = nullptr;   // An LV_SYMBOL_* string, or none
        Mark mark = Mark::None;
        Tone tone = Tone::Normal;
    };

    struct Page {
        std::string title;            // Title bar
        std::string info;             // Status lines under the title ("Connected: Cafe")
        std::vector<Row> rows;        // A menu: pick one
        std::string question;         // A prompt: typed answer ("" for a menu)
        std::string hint;             // How to answer ("Enter = keep, - = clear")
        bool secret = false;          // The answer is a password: masked, with a Show toggle
        std::string value;            // The answer's starting text
        size_t max_len = 0;           // Longest answer (0: no limit)
        bool busy = false;            // Something is in progress: a spinner under the info
        int selected = 0;             // Row highlighted first (the default answer)
    };

    // Shorthands for building rows
    Row row(const std::string& key, const std::string& label, const char* icon = nullptr,
            const std::string& detail = "", Mark mark = Mark::None);
    Row back_row(const std::string& label = "Back");   // Key "0"

    // Index of the row answering `key` (case-insensitive), -1 if none
    int find_row(const Page& page, const std::string& key);
    // The highlight after moving from `from`: direction > 0 up (previous row), < 0 down.
    // Stops at the first and last row; -1 when the page has no rows.
    int move(const Page& page, int from, int direction);
    // `selected` limited to the rows (0 if out of range, -1 without rows)
    int clamp_selection(const Page& page, int selected);

    // Messages printed since the user's last answer, shown on the next page (results, errors,
    // a station list that failed to load). Bounded: keeps the newest lines within MAX_CHARS; no
    // allocation once its buffer has grown (it is fed from every append to the log).
    class Notice
    {
    public:
        static const size_t MAX_CHARS = 1024;
        void add(const char* text);
        void clear();
        // Without leading/trailing blank lines
        std::string text() const;
        bool empty() const;
    private:
        std::string buf;
    };

    // WiFi signal in words for a scan result's RSSI (dBm)
    const char* signal_words(int rssi);
}

#endif
