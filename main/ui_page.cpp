/*
 * UI Page Implementation
 * Row shorthands, highlight movement and the notice buffer behind every menu page.
 * Kept free of ESP-IDF and LVGL headers so the host unit tests (test/host) build it as-is.
 * Shared with SimplrSSH (github.com/MicroHealthLLC/SimplrSSH, MIT).
 */

#include "ui_page.hpp"
#include <cctype>
#include <cstring>

namespace ui
{

// LV_SYMBOL_LEFT (lvgl/src/font/lv_symbol_def.h), spelled out so no LVGL header is needed
static const char* const SYMBOL_LEFT = "\xEF\x81\x93";

Row row(const std::string& key, const std::string& label, const char* icon,
        const std::string& detail, Mark mark)
{
    Row r;
    r.key = key;
    r.label = label;
    r.detail = detail;
    r.icon = icon;
    r.mark = mark;
    return r;
}

Row back_row(const std::string& label)
{
    Row r = row("0", label, SYMBOL_LEFT);
    r.tone = Tone::Back;
    return r;
}

static bool same_key(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

int find_row(const Page& page, const std::string& key)
{
    for (size_t i = 0; i < page.rows.size(); i++) {
        if (same_key(page.rows[i].key, key)) {
            return (int)i;
        }
    }
    return -1;
}

int move(const Page& page, int from, int direction)
{
    int count = (int)page.rows.size();
    if (count == 0) {
        return -1;
    }
    if (from < 0 || from >= count) {
        return 0;   // Nothing highlighted yet: start at the first row
    }
    if (direction > 0) {
        return from > 0 ? from - 1 : 0;
    }
    if (direction < 0) {
        return from < count - 1 ? from + 1 : count - 1;
    }
    return from;
}

int clamp_selection(const Page& page, int selected)
{
    if (page.rows.empty()) {
        return -1;
    }
    if (selected < 0 || selected >= (int)page.rows.size()) {
        return 0;
    }
    return selected;
}

// ---------------------------------------------------------------------------
// Notice
// ---------------------------------------------------------------------------

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

void Notice::add(const char* text)
{
    if (text == nullptr) {
        return;
    }
    // noopengrep: mh.gitlab.c_buffer_rule-strlen-wcslen - text is a C string the firmware built (a literal or std::string::c_str()); the notice keeps a bounded tail
    size_t len = strlen(text);
    if (len == 0) {
        return;
    }
    // One allocation for the notice's lifetime: the buffer never holds more than
    // MAX_CHARS plus one add of at most MAX_CHARS
    if (buf.capacity() < 2 * MAX_CHARS) {
        buf.reserve(2 * MAX_CHARS);
    }
    if (len > MAX_CHARS) {
        text += len - MAX_CHARS;   // Only its newest part fits
        len = MAX_CHARS;
        buf.clear();
    }
    buf.append(text, len);
    if (buf.size() > MAX_CHARS) {
        // Drop the oldest whole lines; a line longer than the excess goes entirely
        size_t excess = buf.size() - MAX_CHARS;
        size_t nl = buf.find('\n', excess - 1);
        buf.erase(0, nl == std::string::npos ? excess : nl + 1);
    }
}

void Notice::clear()
{
    buf.clear();
}

std::string Notice::text() const
{
    size_t first = 0;
    while (first < buf.size() && is_space(buf[first])) {
        first++;
    }
    if (first == buf.size()) {
        return std::string();
    }
    // Keep the first line's indentation: start right after the last blank line
    size_t start = buf.rfind('\n', first);
    start = start == std::string::npos ? 0 : start + 1;
    size_t end = buf.size();
    while (end > first && is_space(buf[end - 1])) {
        end--;
    }
    return buf.substr(start, end - start);
}

bool Notice::empty() const
{
    for (char c : buf) {
        if (!is_space(c)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Signal
// ---------------------------------------------------------------------------

// WiFi signal in words, for the scan list
const char* signal_words(int rssi)
{
    if (rssi >= -55) return "Strong";
    if (rssi >= -67) return "Good";
    if (rssi >= -75) return "Fair";
    return "Weak";
}

}
