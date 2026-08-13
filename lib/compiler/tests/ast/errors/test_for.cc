#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Non-terminated iterables") {
    helpers::test_parser_fail("for (0..4 |i| { a; } else return b;",
                              syntax::diagnostic{"Expected token RBRACE, found IDENT",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 16UZ}},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 19UZ}});
}

TEST_CASE("Missing iterables") {
    helpers::test_parser_fail("for () |i| { a; } else return b;",
                              syntax::diagnostic{"For loops must contain at least one iterable",
                                                 syntax::error::FOR_MISSING_ITERABLES,
                                                 std::pair{0UZ, 0UZ}},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 16UZ}});

    helpers::test_parser_fail(
        "for |i| { a; } else return b;",
        syntax::diagnostic{
            "Expected token LPAREN, found BW_OR", syntax::error::UNEXPECTED_TOKEN, 0, 4},
        syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 13UZ}});
}

TEST_CASE("Non-terminated captures") {
    helpers::test_parser_fail(
        "for (0..4) |i { a; } else return b;",
        syntax::diagnostic{
            "Expected token COMMA, found LBRACE", syntax::error::UNEXPECTED_TOKEN, 0, 14},
        syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 19UZ}});
}

TEST_CASE("Missing captures") {
    helpers::test_parser_fail(
        "for (0..4) { a; } else return b;",
        syntax::diagnostic{"For loops must contain the same number of captures iterables, "
                           "which can be discarded with an underscore",
                           syntax::error::FOR_ITERABLE_CAPTURE_MISMATCH,
                           std::pair{0UZ, 0UZ}},
        syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 16UZ}});
}

TEST_CASE("Illegal capture") {
    helpers::test_parser_fail("for (0..4) |2| { a; } else return b;",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 12},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 20UZ}});
}

TEST_CASE("Iterable-capture mismatch") {
    helpers::test_parser_fail(
        "for (0..4) |i, j| { a; } else return b;",
        syntax::diagnostic{"The number of for loop captures must match the number of iterables",
                           syntax::error::FOR_ITERABLE_CAPTURE_MISMATCH,
                           std::pair{0UZ, 0UZ}});
}

TEST_CASE("Illegal for-else clause") {
    helpers::test_parser_fail("for (0..4) |i| { a; } else import std;",
                              syntax::diagnostic{syntax::error::ILLEGAL_LOOP_NON_BREAK, 0, 27});
}

} // namespace ghoti::tests
