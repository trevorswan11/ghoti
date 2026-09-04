#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] constexpr auto overflow_error(syntax::error error) -> syntax::diagnostic {
    return syntax::diagnostic{"Overflow of literal", error, 0, 0};
}

} // namespace

TEST_CASE("Integer literal overflow past the 128-bit compile-time cap") {
    // 33 hex digits => 132 bits, past the u128 evaluation domain.
    helpers::test_parser_fail("0x1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0x1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFu64;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0x1FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFz;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
}

TEST_CASE("Removed and malformed literal suffixes are rejected") {
    const auto err = [](std::string_view msg, syntax::error e) {
        return syntax::diagnostic{std::string{msg}, e, 0, 0};
    };
    constexpr auto needs_width{"integer literal suffix needs a width, e.g. 42u8"};

    helpers::test_parser_fail("42u;", err(needs_width, syntax::error::INVALID_NUMBER_LITERAL));
    helpers::test_parser_fail("42i;", err(needs_width, syntax::error::INVALID_NUMBER_LITERAL));
    helpers::test_parser_fail("42u0;", err(needs_width, syntax::error::INVALID_NUMBER_LITERAL));
    helpers::test_parser_fail(
        "1.0f;",
        err("float literal suffix needs a width, e.g. 1.0f32; valid widths are 16/32/64/80/128",
            syntax::error::FLOAT_OVERFLOW));
}

TEST_CASE("Character escape errors") {
    helpers::test_parser_fail("'\\f';",
                              syntax::diagnostic{syntax::error::UNKNOWN_CHARACTER_ESCAPE, 0, 0});
}

TEST_CASE("Floating point overflow") {
    helpers::test_parser_fail("1023.234612e234000f64;",
                              overflow_error(syntax::error::DOUBLE_OVERFLOW));
    helpers::test_parser_fail("1023.234612e234000;",
                              overflow_error(syntax::error::DOUBLE_OVERFLOW));
}

TEST_CASE("Literal digit span too long for the scratch buffer overflows cleanly") {
    const auto too_long{std::string(1'200, '1') + ";"};
    helpers::test_parser_fail(too_long, overflow_error(syntax::error::INTEGER_OVERFLOW));
}

} // namespace ghoti::tests
