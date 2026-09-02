#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("If without condition") {
    helpers::test_parser_fail("if () b;",
                              syntax::diagnostic{"If expressions must have a condition",
                                                 syntax::error::IF_MISSING_CONDITION,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("If with illegal consequence") {
    helpers::test_parser_fail("if (a) import std;",
                              syntax::diagnostic{syntax::error::ILLEGAL_IF_BRANCH, 0, 7});
}

TEST_CASE("If with illegal alternate") {
    helpers::test_parser_fail("if (a) {} else import std;",
                              syntax::diagnostic{syntax::error::ILLEGAL_IF_BRANCH, 0, 15});
}

TEST_CASE("Match without condition") {
    helpers::test_parser_fail("match () { b => c, };",
                              syntax::diagnostic{"Match expressions must have a condition",
                                                 syntax::error::MATCH_EXPR_MISSING_CONDITION,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail(
        "match { b => c, };",
        syntax::diagnostic{
            "Expected token LPAREN, found LBRACE", syntax::error::UNEXPECTED_TOKEN, 0, 6});
}

TEST_CASE("Armless match expression") {
    helpers::test_parser_fail("match (a) {};",
                              syntax::diagnostic{"Match expressions must have at least one arm",
                                                 syntax::error::ARMLESS_MATCH_EXPR,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("Malformed arm pattern") {
    helpers::test_parser_fail(
        "match {  => c, };",
        syntax::diagnostic{
            "Expected token LPAREN, found LBRACE", syntax::error::UNEXPECTED_TOKEN, 0, 6});

    helpers::test_parser_fail(
        "match (a) { for (0..3) |i| { var a: i32 = undefined; } => |b| c };",
        syntax::diagnostic{"Unmatchable expression 'for loop' used as a match arm pattern",
                           syntax::error::ILLEGAL_MATCH_PATTERN,
                           std::pair{0UZ, 12UZ}});
}

TEST_CASE("Illegal match arm dispatch") {
    helpers::test_parser_fail("match (a) { b => import std; };",
                              syntax::diagnostic{syntax::error::ILLEGAL_MATCH_ARM, 0, 17});
}

TEST_CASE("Arm missing fat arrow") {
    helpers::test_parser_fail(
        "match (a) { b c, };",
        syntax::diagnostic{
            "Expected token FAT_ARROW, found IDENT", syntax::error::UNEXPECTED_TOKEN, 0, 14});
}

TEST_CASE("Arm separated by semicolon instead of comma") {
    helpers::test_parser_fail(
        "match (a) { b => c; c => d, };",
        syntax::diagnostic{
            "Semicolon is not allowed in this context", syntax::error::UNEXPECTED_TOKEN, 0, 18});
}

TEST_CASE("Illegal match catch-all") {
    helpers::test_parser_fail(
        "match (a) { b => c, _ => f, _ => g, };",
        syntax::diagnostic{
            "Duplicate catch-all match arm", syntax::error::ILLEGAL_MATCH_CATCH_ALL, 0, 28});

    helpers::test_parser_fail(
        "match (a) { _ => |b| c, };",
        syntax::diagnostic{"Catch-all match arms may not have a capture clause",
                           syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                           std::pair{0UZ, 18UZ}});
}

TEST_CASE("Catch-all cannot be part of a multi-value match arm") {
    helpers::test_parser_fail(
        "match (a) { _, b => c, };",
        syntax::diagnostic{"A catch-all '_' arm cannot list additional patterns",
                           syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                           0,
                           15});
    helpers::test_parser_fail(
        "match (a) { b, _ => c, };",
        syntax::diagnostic{"A catch-all '_' arm cannot list additional patterns",
                           syntax::error::ILLEGAL_MATCH_CATCH_ALL,
                           0,
                           15});
}

TEST_CASE("Illegal match arm capture modifier on a discarded capture") {
    helpers::test_parser_fail("match (a) { b => |&mut _| c, };",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 23});

    helpers::test_parser_fail("match (a) { b => |^ _| c, };",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 20});
}

} // namespace ghoti::tests
