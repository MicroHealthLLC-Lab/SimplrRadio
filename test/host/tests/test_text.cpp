/*
 * text_to_ascii (text.c): station names, song titles and SSIDs from the network become
 * printable ASCII for the screen's fonts, bounded and without control characters.
 */

#include "text.h"
#include "test.hpp"
#include <string>

static std::string ascii(const std::string& in, size_t cap = 256)
{
    char out[512];
    text_to_ascii(out, cap, in.c_str());
    return out;
}

TEST(text_plain_ascii_unchanged)
{
    CHECK_EQ(ascii("KUTX 98.9 - Austin"), std::string("KUTX 98.9 - Austin"));
}

TEST(text_typographic_punctuation)
{
    CHECK_EQ(ascii("Don\xE2\x80\x99t Stop \xE2\x80\x94 \xE2\x80\x9CLive\xE2\x80\x9D\xE2\x80\xA6"),
             std::string("Don't Stop - \"Live\"..."));
}

TEST(text_accented_letters_fold)
{
    CHECK_EQ(ascii("Caf\xC3\xA9 M\xC3\xBCller \xC3\x85ngstr\xC3\xB6m Stra\xC3\x9F" "e \xC5\x81\xC3\xB3" "d\xC5\xBA"),
             std::string("Cafe Muller Angstrom Strasse Lodz"));
    CHECK_EQ(ascii("\xC3\x86on \xC3\x98resund"), std::string("AEon Oresund"));
}

TEST(text_other_scripts_one_question_mark_each)
{
    CHECK_EQ(ascii("Radio \xE6\x9D\xB1\xE4\xBA\xAC"), std::string("Radio ??"));   // Two CJK characters
    CHECK_EQ(ascii("\xF0\x9F\x8E\xB5 Hits"), std::string("? Hits"));              // An emoji
}

TEST(text_controls_and_spaces_collapse)
{
    CHECK_EQ(ascii("  A\t\tB\r\nC\x1B[31m  "), std::string("A B C [31m"));
    CHECK_EQ(ascii("x\xC2\xA0y\xE2\x80\x8Bz"), std::string("x yz"));   // nbsp, zero-width space
}

TEST(text_malformed_utf8)
{
    CHECK_EQ(ascii("a\x80" "b\xC3"), std::string("a?b?"));        // Stray byte, truncated sequence
    CHECK_EQ(ascii("\xE2\x80" "x"), std::string("??x"));       // Each byte of a broken sequence
}

TEST(text_bounded)
{
    CHECK_EQ(ascii("abcdef", 4), std::string("abc"));
    CHECK_EQ(ascii("ab\xE2\x80\xA6", 4), std::string("ab"));      // "..." doesn't fit whole
    char one[1] = {'x'};
    text_to_ascii(one, 1, "abc");
    CHECK_EQ(one[0], '\0');
    text_to_ascii(one, 1, nullptr);
    CHECK_EQ(one[0], '\0');
}
