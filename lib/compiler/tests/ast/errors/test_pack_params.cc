#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("A parameter pack must be the last parameter") {
    helpers::test_parser_fail("const f = fn(rest..., x: i32): void {};",
                              syntax::diagnostic{"A parameter pack must be the last parameter",
                                                 syntax::error::PACK_PARAM_NOT_LAST,
                                                 std::pair{0UZ, 20UZ}});
}

TEST_CASE("`unroll for` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "unroll for (a) |v| { b; } else return c;",
        syntax::diagnostic{"`unroll for` cannot have an `else`/non-break clause",
                           syntax::error::UNROLLED_LOOP_HAS_ELSE,
                           std::pair{0UZ, 26UZ}});
}

TEST_CASE("`unroll while` cannot have an else/non-break clause") {
    helpers::test_parser_fail(
        "unroll while (a) { b; } else return c;",
        syntax::diagnostic{"`unroll while` cannot have an `else`/non-break clause",
                           syntax::error::UNROLLED_LOOP_HAS_ELSE,
                           std::pair{0UZ, 24UZ}});
}

TEST_CASE("`unroll for` cannot be labeled") {
    helpers::test_parser_fail(
        "blk: unroll for (a) |v| { b; };",
        syntax::diagnostic{"`unroll for` cannot be labeled; it has no `break`/`continue` to "
                           "target",
                           syntax::error::UNROLLED_LOOP_LABELED,
                           std::pair{0UZ, 5UZ}});
}

TEST_CASE("`unroll` must be followed by a loop") {
    helpers::test_parser_fail(
        "unroll if (a) { b; };",
        syntax::diagnostic{"Expected a `for`, `while`, `do`, or `loop` after 'unroll'",
                           syntax::error::UNROLL_MISSING_LOOP,
                           std::pair{0UZ, 0UZ}});
}

} // namespace ghoti::tests
