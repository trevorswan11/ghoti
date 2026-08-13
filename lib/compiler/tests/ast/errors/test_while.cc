#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Missing do-while condition") {
    helpers::test_parser_fail("do {a; } while ();",
                              syntax::diagnostic{"While loops must have a condition",
                                                 syntax::error::WHILE_MISSING_CONDITION,
                                                 std::pair{0UZ, 15UZ}});
}

TEST_CASE("Unclosed do-while body") {
    helpers::test_parser_fail("do { while (true);",
                              syntax::diagnostic{"Expected token LBRACE, found SEMICOLON",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 17UZ}});
}

TEST_CASE("Unclosed do-while condition") {
    helpers::test_parser_fail("do {a; } while (true;",
                              syntax::diagnostic{"Expected token RPAREN, found SEMICOLON",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 20UZ}});
}

TEST_CASE("Missing while condition") {
    helpers::test_parser_fail("while () {a;};",
                              syntax::diagnostic{"While loops must have a corresponding condition",
                                                 syntax::error::WHILE_MISSING_CONDITION,
                                                 std::pair{0UZ, 0UZ}},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 12UZ}});
}

TEST_CASE("Unclosed while body") {
    helpers::test_parser_fail("while (true) {;",
                              syntax::diagnostic{"No prefix parse function for SEMICOLON(;) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 14UZ}});
}

TEST_CASE("Unclosed while condition") {
    helpers::test_parser_fail("while (true {};",
                              syntax::diagnostic{"Expected token RPAREN, found SEMICOLON",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 14UZ}});
}

TEST_CASE("Malformed while continuation") {
    helpers::test_parser_fail(
        "while (true) : () {};",
        syntax::diagnostic{"Continuation expression was expected but not found",
                           syntax::error::EMPTY_WHILE_CONTINUATION,
                           std::pair{0UZ, 11UZ}});

    helpers::test_parser_fail("while (true) : (i += 1 {};",
                              syntax::diagnostic{"Expected token RPAREN, found SEMICOLON",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 25UZ}});
}

TEST_CASE("Illegal while-else clause") {
    helpers::test_parser_fail("while (true) : (i += 1) {} else import std;",
                              syntax::diagnostic{syntax::error::ILLEGAL_LOOP_NON_BREAK, 0, 32});

    helpers::test_parser_fail("while (true) : (i += 1) {} else;",
                              syntax::diagnostic{"No prefix parse function for SEMICOLON(;) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 31UZ}});
}

} // namespace ghoti::tests
