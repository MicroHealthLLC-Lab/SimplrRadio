/*
 * UI pages (ui_page.cpp): row shorthands, highlight movement over a menu's rows, the bounded
 * notice fed from every append to the log, and the title bar's status words.
 */

#include "ui_page.hpp"
#include "test.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>

using namespace ui;

static Page menu(size_t count)
{
    Page p;
    for (size_t i = 0; i < count; i++) {
        p.rows.push_back(row(std::to_string(i + 1), "Row " + std::to_string(i + 1)));
    }
    return p;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

TEST(ui_row_shorthand)
{
    Row r = row("2", "Saved networks", "ICON", "3", Mark::More);
    CHECK_EQ(r.key, std::string("2"));
    CHECK_EQ(r.label, std::string("Saved networks"));
    CHECK_EQ(r.detail, std::string("3"));
    CHECK_EQ(std::string(r.icon), std::string("ICON"));
    CHECK(r.mark == Mark::More);
    CHECK(r.tone == Tone::Normal);

    Row plain = row("y", "Delete");
    CHECK(plain.icon == nullptr);
    CHECK(plain.detail.empty());
    CHECK(plain.mark == Mark::None);
}

TEST(ui_back_row)
{
    Row r = back_row();
    CHECK_EQ(r.key, std::string("0"));
    CHECK_EQ(r.label, std::string("Back"));
    CHECK(r.tone == Tone::Back);
    CHECK(r.mark == Mark::None);
    CHECK(r.icon != nullptr);
    CHECK_EQ(std::string(r.icon), std::string("\xEF\x81\x93"));   // LV_SYMBOL_LEFT
    CHECK_EQ(back_row("Cancel").label, std::string("Cancel"));
}

TEST(ui_find_row_ignores_case)
{
    Page p;
    p.rows = {row("1", "One"), row("t", "Test"), row("Ab", "Mixed"), back_row()};
    CHECK_EQ(find_row(p, "1"), 0);
    CHECK_EQ(find_row(p, "T"), 1);
    CHECK_EQ(find_row(p, "t"), 1);
    CHECK_EQ(find_row(p, "aB"), 2);
    CHECK_EQ(find_row(p, "0"), 3);
    CHECK_EQ(find_row(p, "x"), -1);
    CHECK_EQ(find_row(p, ""), -1);
    CHECK_EQ(find_row(p, " 1"), -1);   // Nothing is trimmed
    CHECK_EQ(find_row(p, "A"), -1);    // Whole keys only
    CHECK_EQ(find_row(Page(), "1"), -1);
}

// ---------------------------------------------------------------------------
// Highlight
// ---------------------------------------------------------------------------

TEST(ui_move_steps_and_stops_at_the_ends)
{
    Page p = menu(3);
    CHECK_EQ(move(p, 0, -1), 1);   // Down
    CHECK_EQ(move(p, 1, -1), 2);
    CHECK_EQ(move(p, 2, -1), 2);   // No wrap at the bottom
    CHECK_EQ(move(p, 2, 1), 1);    // Up
    CHECK_EQ(move(p, 1, 1), 0);
    CHECK_EQ(move(p, 0, 1), 0);    // No wrap at the top
    CHECK_EQ(move(p, 1, 0), 1);    // Stay
    CHECK_EQ(move(p, 1, 5), 0);    // Only the sign counts
    CHECK_EQ(move(p, 1, -5), 2);
}

TEST(ui_move_from_nothing_highlighted)
{
    Page p = menu(3);
    CHECK_EQ(move(p, -1, -1), 0);
    CHECK_EQ(move(p, -1, 1), 0);
    CHECK_EQ(move(p, 3, -1), 0);
    CHECK_EQ(move(p, 99, 1), 0);
    CHECK_EQ(move(p, -1, 0), 0);
}

TEST(ui_move_without_rows)
{
    Page p;
    CHECK_EQ(move(p, 0, 1), -1);
    CHECK_EQ(move(p, -1, -1), -1);
    CHECK_EQ(move(p, 0, 0), -1);
    Page one = menu(1);
    CHECK_EQ(move(one, 0, 1), 0);
    CHECK_EQ(move(one, 0, -1), 0);
}

TEST(ui_clamp_selection)
{
    Page p = menu(3);
    CHECK_EQ(clamp_selection(p, 0), 0);
    CHECK_EQ(clamp_selection(p, 2), 2);
    CHECK_EQ(clamp_selection(p, 3), 0);
    CHECK_EQ(clamp_selection(p, -1), 0);
    CHECK_EQ(clamp_selection(Page(), 0), -1);
    CHECK_EQ(clamp_selection(Page(), 5), -1);
}

// ---------------------------------------------------------------------------
// Notice
// ---------------------------------------------------------------------------

TEST(ui_notice_collects_and_clears)
{
    Notice n;
    CHECK(n.empty());
    CHECK_EQ(n.text(), std::string());
    n.add("Saved.\n");
    n.add("Connected to Cafe\n");
    CHECK(!n.empty());
    CHECK_EQ(n.text(), std::string("Saved.\nConnected to Cafe"));
    n.clear();
    CHECK(n.empty());
    CHECK_EQ(n.text(), std::string());
}

TEST(ui_notice_ignores_null_and_empty)
{
    Notice n;
    n.add(nullptr);
    n.add("");
    CHECK(n.empty());
    n.add("x");
    n.add(nullptr);
    CHECK_EQ(n.text(), std::string("x"));
}

TEST(ui_notice_strips_blank_lines_at_the_ends_only)
{
    Notice n;
    n.add("\n\n  \t\n");
    CHECK(n.empty());
    CHECK_EQ(n.text(), std::string());
    n.add("  Fingerprint:\n\n   SHA256:abc  \n");
    n.add("\r\n \n");
    CHECK(!n.empty());
    // Inner blank line and the first line's indentation stay
    CHECK_EQ(n.text(), std::string("  Fingerprint:\n\n   SHA256:abc"));
}

TEST(ui_notice_drops_oldest_whole_lines)
{
    Notice n;
    std::string line(99, 'a');   // 100 chars with its newline
    for (int i = 0; i < 10; i++) {
        line[0] = (char)('0' + i);
        n.add((line + "\n").c_str());
    }
    CHECK_EQ(n.text().size(), (size_t)999);   // 1000 chars, trailing newline stripped
    // 30 more chars: the oldest line goes entirely, not just 6 of its chars
    n.add("bbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n");
    std::string t = n.text();
    CHECK_EQ(t[0], '1');
    CHECK_EQ(t.size(), (size_t)929);
    CHECK_EQ(t.substr(t.size() - 29), std::string(29, 'b'));
}

TEST(ui_notice_hard_cuts_one_long_line)
{
    Notice n;
    n.add(std::string(Notice::MAX_CHARS, 'a').c_str());
    n.add("bcd");
    std::string t = n.text();
    CHECK_EQ(t.size(), Notice::MAX_CHARS);
    CHECK_EQ(t.substr(t.size() - 3), std::string("bcd"));
    CHECK_EQ(t[0], 'a');
}

TEST(ui_notice_oversize_add_keeps_its_tail)
{
    Notice n;
    n.add("old line\n");
    std::string big(3 * Notice::MAX_CHARS, 'x');
    big += "END";
    n.add(big.c_str());
    std::string t = n.text();
    CHECK_EQ(t.size(), Notice::MAX_CHARS);
    CHECK(t.find("old") == std::string::npos);
    CHECK_EQ(t.substr(t.size() - 3), std::string("END"));
}

TEST(ui_notice_stays_bounded)
{
    Notice n;
    n.add("a");   // Reserves the buffer
    std::string chunk;
    chunk.reserve(4 * Notice::MAX_CHARS);
    // Feed far more than the buffer in mixed sizes, some over MAX_CHARS: the contents stay
    // within MAX_CHARS
    size_t longest = 0;
    for (int i = 0; i < 5000; i++) {
        chunk.assign((size_t)(i * 37 % 300) + (i % 500 == 0 ? 3 * Notice::MAX_CHARS : 0),
                     (char)('a' + i % 26));
        if (i % 3 == 0) {
            chunk += '\n';
        }
        n.add(chunk.c_str());
        if (i % 7 == 0) {
            n.clear();   // Keeps its capacity
        }
    }
    CHECK(n.text().size() <= Notice::MAX_CHARS);
    for (int i = 0; i < 200; i++) {
        n.add("line of text\n");
        longest = std::max(longest, n.text().size());
    }
    CHECK(longest <= Notice::MAX_CHARS);
    CHECK(!n.empty());
}
