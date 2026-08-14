#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Function missing return type") {
    helpers::test_parser_fail(
        "fn(^mut this, a: A, b: ^B, );",
        syntax::diagnostic{
            "Expected token COLON, found SEMICOLON", syntax::error::UNEXPECTED_TOKEN, 0, 28});

    helpers::test_parser_fail("fn(^mut this, a: A, b: ^B, ): ;",
                              syntax::diagnostic{"No prefix parse function for SEMICOLON(;) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 30UZ}});

    helpers::test_parser_fail("fn(^mut this, a: A, b: ^B, ): ",
                              syntax::diagnostic{syntax::error::MISSING_EXPLICIT_TYPE, 0, 28});
}

TEST_CASE("Function parameter missing type") {
    helpers::test_parser_fail(
        "fn(^mut this, a): i32;",
        syntax::diagnostic{
            "Expected token COLON, found RPAREN", syntax::error::UNEXPECTED_TOKEN, 0, 15});
}

TEST_CASE("Out-of-place self parameter") {
    helpers::test_parser_fail("fn(a: A, &self): i32;",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 9});

    helpers::test_parser_fail(
        "fn(a: A, self): i32;",
        syntax::diagnostic{
            "Expected token COLON, found RPAREN", syntax::error::UNEXPECTED_TOKEN, 0, 13});
}

TEST_CASE("Illegal self parameter modifier") {
    using namespace std::string_view_literals;
    const auto modifier{GENERATE("volatile"sv, "mut volatile"sv)};

    helpers::test_parser_fail(
        fmt::format("fn({} self): i32 {{}};", modifier),
        syntax::diagnostic{
            "Self parameters cannot be marked volatile; they must be values, refs, or pointers",
            syntax::error::ILLEGAL_SELF_PARAMETER_MODIFIER,
            std::pair{0UZ, 3UZ}});
}

TEST_CASE("Out-of-place variadic parameter") {
    helpers::test_parser_fail(
        "fn(a: A, ..., b: B): i32;",
        syntax::diagnostic{
            "Expected token RPAREN, found IDENT", syntax::error::UNEXPECTED_TOKEN, 0, 14});
}

TEST_CASE("Default function parameter") {
    helpers::test_parser_fail("fn(a: A = 2): i32;",
                              syntax::diagnostic{"Function parameters may not have default values",
                                                 syntax::error::FN_PARAMETER_HAS_DEFAULT_VALUE,
                                                 std::pair{0UZ, 6UZ}});
}

TEST_CASE("Noreturn function types") {
    helpers::test_parser_fail("fn(a: &noreturn): i32;",
                              syntax::diagnostic{"Explicit `noreturn` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_NORETURN_TYPE_MODIFIER,
                                                 std::pair{0UZ, 6UZ}});

    helpers::test_parser_fail(
        "fn(a: noreturn): i32;",
        syntax::diagnostic{"Function parameter types may not be marked `noreturn`",
                           syntax::error::FN_PARAMETER_IS_NORETURN,
                           std::pair{0UZ, 6UZ}});

    helpers::test_parser_fail("fn(a: A): &noreturn;",
                              syntax::diagnostic{"Explicit `noreturn` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_NORETURN_TYPE_MODIFIER,
                                                 std::pair{0UZ, 10UZ}});
}

TEST_CASE("Illegal type function types") {
    const auto expected_diag = [](usize col) -> syntax::diagnostic {
        return {"Explicit `type` type cannot have a modifier",
                syntax::error::ILLEGAL_TYPE_TYPE_MODIFIER,
                std::pair{0UZ, col}};
    };

    helpers::test_parser_fail("fn(A: &type): i32;", expected_diag(6));
    helpers::test_parser_fail("fn(A: type): &type;", expected_diag(13));
}

TEST_CASE("Non-terminated parameter list") {
    helpers::test_parser_fail("fn(a: A, : i32;",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 9});
}

TEST_CASE("'move' must be followed directly by 'fn'") {
    helpers::test_parser_fail("move struct {};",
                              syntax::diagnostic{"'move' may only appear directly before 'fn'",
                                                 syntax::error::ILLEGAL_MOVE_USAGE,
                                                 std::pair{0UZ, 0UZ}});
}

} // namespace ghoti::tests
