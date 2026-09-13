#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("A parameter pack must be the last parameter") {
    helpers::test_parser_fail("const f := fn(rest..., x: i32): void {};",
                              syntax::diagnostic{"A parameter pack must be the last parameter",
                                                 syntax::error::PACK_PARAM_NOT_LAST,
                                                 std::pair{0UZ, 21UZ}});
}

TEST_CASE("`for constexpr` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "for constexpr (a) |v| { b; } else return c;",
        syntax::diagnostic{"`for constexpr` cannot have an `else`/non-break clause",
                           syntax::error::CONSTEXPR_LOOP_HAS_ELSE,
                           std::pair{0UZ, 29UZ}});
}

TEST_CASE("`while constexpr` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "while constexpr (a) { b; } else return c;",
        syntax::diagnostic{"`while constexpr` cannot have an `else`/non-break clause",
                           syntax::error::CONSTEXPR_LOOP_HAS_ELSE,
                           std::pair{0UZ, 27UZ}});
}

TEST_CASE("`for constexpr` cannot be labeled") {
    helpers::test_parser_fail(
        "blk: for constexpr (a) |v| { b; };",
        syntax::diagnostic{"`for constexpr` cannot be labeled; it has no `break`/`continue` to "
                           "target",
                           syntax::error::CONSTEXPR_LOOP_LABELED,
                           std::pair{0UZ, 5UZ}});
}

} // namespace ghoti::tests
