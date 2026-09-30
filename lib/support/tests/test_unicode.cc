#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/source_file.hh"
#include "support/test.hh"
#include "support/unicode.hh"

namespace ghoti::tests {

namespace {

// é as one precomposed code point, and as e + a combining acute accent
constexpr std::string_view PRECOMPOSED{"\xC3\xA9"};
constexpr std::string_view DECOMPOSED{"e\xCC\x81"};
constexpr std::string_view GRINNING{"\xF0\x9F\x98\x80"};
// man, ZWJ, woman, ZWJ, girl
constexpr std::string_view FAMILY{"\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                                  "\xF0\x9F\x91\xA7"};
constexpr std::string_view NI_HAO{"\xE4\xBD\xA0\xE5\xA5\xBD"};
// regional indicators U and S
constexpr std::string_view FLAG{"\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"};
// a heart with the emoji presentation selector
constexpr std::string_view HEART{"\xE2\x9D\xA4\xEF\xB8\x8F"};

} // namespace

TEST_CASE("display width counts terminal columns") {
    CHECK(display_width("abc") == 3);
    CHECK(display_width(PRECOMPOSED) == 1);
    CHECK(display_width(DECOMPOSED) == 1);
    CHECK(display_width(GRINNING) == 2);
    CHECK(display_width(FAMILY) == 2);
    CHECK(display_width(NI_HAO) == 4);
    CHECK(display_width(FLAG) == 2);
    CHECK(display_width(HEART) == 1);
    // A malformed byte is one column, and doesn't swallow what follows
    CHECK(display_width("\xFF"
                        "a") == 2);
}

TEST_CASE("UTF-16 lengths and offsets split nothing") {
    CHECK(utf16_length("abc") == 3);
    CHECK(utf16_length(GRINNING) == 2);
    CHECK(utf16_length(NI_HAO) == 2);
    CHECK(byte_offset_of_utf16(GRINNING, 2) == 4);
    CHECK(byte_offset_of_utf16(NI_HAO, 1) == 3);
    // Past the end clamps, and half a surrogate pair rounds to the whole code point
    CHECK(byte_offset_of_utf16("ab", 9) == 2);
    CHECK(byte_offset_of_utf16(GRINNING, 1) == 4);
}

TEST_CASE("a diagnostic caret lines up under text after wide characters") {
    const auto caret_under{[](std::string_view line, usize byte_column) {
        const source_file file{line};
        return UNWRAP(file.get_diagnostic_strings(source_location{0, byte_column}).second);
    }};
    // `x` after a two-column emoji sits at display column 6
    CHECK(caret_under("a = \xF0\x9F\x98\x80x;", 8) == "      ^");
    // after two CJK characters (six bytes, four columns)
    CHECK(caret_under("\xE4\xBD\xA0\xE5\xA5\xBD + y", 9) == "       ^");
    // a combining mark takes no column
    CHECK(caret_under("e\xCC\x81 z", 4) == "  ^");
    // a tab is copied so the caret lines up in any tab width
    CHECK(caret_under("w\t\xF0\x9F\x98\x80 z", 7) == " \t   ^");
}

} // namespace ghoti::tests
