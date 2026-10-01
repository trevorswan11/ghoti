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

TEST_CASE("`for comptime` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "for comptime (a) |v| { b; } else return c;",
        syntax::diagnostic{"`for comptime` cannot have an `else`/non-break clause",
                           syntax::error::COMPTIME_LOOP_HAS_ELSE,
                           std::pair{0UZ, 28UZ}});
}

TEST_CASE("`while comptime` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "while comptime (a) { b; } else return c;",
        syntax::diagnostic{"`while comptime` cannot have an `else`/non-break clause",
                           syntax::error::COMPTIME_LOOP_HAS_ELSE,
                           std::pair{0UZ, 26UZ}});
}

TEST_CASE("`for comptime` cannot be labeled") {
    helpers::test_parser_fail(
        "blk: for comptime (a) |v| { b; };",
        syntax::diagnostic{"`for comptime` cannot be labeled; it has no `break`/`continue` to "
                           "target",
                           syntax::error::COMPTIME_LOOP_LABELED,
                           std::pair{0UZ, 5UZ}});
}

} // namespace ghoti::tests
