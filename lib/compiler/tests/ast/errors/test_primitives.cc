#include <string>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] constexpr auto overflow_error(syntax::error error) -> syntax::diagnostic {
    return syntax::diagnostic{"Overflow of literal", error, 0, 0};
}

} // namespace

TEST_CASE("Signed integer overflow") {
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFFFF;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFFFFl;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFz;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
}

TEST_CASE("Unsigned integer overflow") {
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFu;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFFul;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("0xFFFFFFFFFFFFFFFFFuz;",
                              overflow_error(syntax::error::INTEGER_OVERFLOW));
    helpers::test_parser_fail("'\\f';",
                              syntax::diagnostic{syntax::error::UNKNOWN_CHARACTER_ESCAPE, 0, 0});
}

TEST_CASE("Floating point overflow") {
    helpers::test_parser_fail("1023.234612e234000f;",
                              overflow_error(syntax::error::FLOAT_OVERFLOW));
    helpers::test_parser_fail("1023.234612e234000;",
                              overflow_error(syntax::error::DOUBLE_OVERFLOW));
}

TEST_CASE("Literal digit span too long for the scratch buffer overflows cleanly") {
    const auto too_long{std::string(1'200, '1') + ";"};
    helpers::test_parser_fail(too_long, overflow_error(syntax::error::INTEGER_OVERFLOW));
}

} // namespace ghoti::tests
