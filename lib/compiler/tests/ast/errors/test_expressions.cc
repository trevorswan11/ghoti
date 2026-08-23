#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Array size token requirement") {
    helpers::test_parser_fail(
        "[]i32{2};",
        syntax::diagnostic{"Array literals must be initialized with an implicit or explicit size",
                           syntax::error::MISSING_ARRAY_SIZE_TOKEN,
                           std::pair{0UZ, 0UZ}});
}

TEST_CASE("No arguments with comma") {
    helpers::test_parser_fail("func(,)",
                              syntax::diagnostic{"A comma implies an argument but none were found",
                                                 syntax::error::COMMA_WITH_MISSING_CALL_ARGUMENT,
                                                 std::pair{0UZ, 5UZ}});
}

TEST_CASE("Non-comma separated arguments") {
    helpers::test_parser_fail(
        "func(1 2)",
        syntax::diagnostic{
            "Expected token COMMA, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 7});
}

TEST_CASE("Non-terminated identifier") {
    helpers::test_parser_fail(
        "foobar",
        syntax::diagnostic{
            "Expected token SEMICOLON, found END", syntax::error::UNEXPECTED_TOKEN, 0, 6});
}

TEST_CASE("No index") {
    helpers::test_parser_fail(
        "arr[]",
        syntax::diagnostic{"Cannot index into an array without an index expression",
                           syntax::error::INDEX_MISSING_EXPRESSION,
                           std::pair{0UZ, 3UZ}});
}

TEST_CASE("Illegal infix node") {
    helpers::test_parser_fail(
        "a and import std;",
        syntax::diagnostic{"No prefix parse function for IMPORT(import) found",
                           syntax::error::MISSING_PREFIX_PARSER,
                           std::pair{0UZ, 6UZ}});
}

TEST_CASE("Non-terminated infix") {
    helpers::test_parser_fail("a and;",
                              syntax::diagnostic{"No prefix parse function for SEMICOLON(;) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 5UZ}});

    helpers::test_parser_fail("a and",
                              syntax::diagnostic{"Infix expressions require a right-hand operand",
                                                 syntax::error::INFIX_MISSING_RHS,
                                                 std::pair{0UZ, 2UZ}});
}

TEST_CASE("Expression nested too deeply") {
    const auto nested{std::string(513, '(')};
    helpers::test_parser_fail(nested + "1" + std::string(513, ')') + ";",
                              syntax::diagnostic{"Expression nested too deeply",
                                                 syntax::error::EXPRESSION_NESTED_TOO_DEEPLY,
                                                 std::pair{0UZ, 512UZ}});
}

TEST_CASE("Illegal tokens report a specific diagnostic instead of a generic prefix-parser one") {
    helpers::test_parser_fail("\"unterminated;",
                              syntax::diagnostic{"Unterminated string literal",
                                                 syntax::error::UNTERMINATED_STRING,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail("0x;",
                              syntax::diagnostic{"Invalid numeric literal",
                                                 syntax::error::INVALID_NUMBER_LITERAL,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail("'';",
                              syntax::diagnostic{"Invalid or unterminated character literal",
                                                 syntax::error::INVALID_CHARACTER_LITERAL,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("Unclosed implicit initializer") {
    helpers::test_parser_fail(".{",
                              syntax::diagnostic{"Expected token RBRACE, found END",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 2UZ}});

    helpers::test_parser_fail(".{ .a = 2",
                              syntax::diagnostic{"Expected token COMMA, found END",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 9UZ}});
}

TEST_CASE("Unclosed explicit initializer") {
    helpers::test_parser_fail("T{",
                              syntax::diagnostic{"Expected token RBRACE, found END",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 2UZ}});

    helpers::test_parser_fail("T{ .a = 2",
                              syntax::diagnostic{"Expected token COMMA, found END",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 9UZ}});
}

TEST_CASE("Malformed initializer key-value") {
    helpers::test_parser_fail("T{ .a = };",
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 8UZ}});
}

TEST_CASE("Non-ident label") {
    helpers::test_parser_fail("2: {};",
                              syntax::diagnostic{"Labels may only be identifiers",
                                                 syntax::error::ILLEGAL_LABEL,
                                                 std::pair{0UZ, 1UZ}});
}

TEST_CASE("Illegal label expressions") {
    const auto expected_diag = [](usize ln = 0UZ, usize col = 3UZ) -> syntax::diagnostic {
        return {"Labeled expressions may only be conditionals or loops",
                syntax::error::ILLEGAL_LABEL_EXPRESSION,
                std::pair{ln, col}};
    };

    helpers::test_parser_fail("a: 3;", expected_diag());
    helpers::test_parser_fail("a: b;", expected_diag());
    helpers::test_parser_fail("a: b();", expected_diag());
    helpers::test_parser_fail("a: b: c: {};", expected_diag(0, 6));
}

TEST_CASE("Illegal label statements") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Labeled statements may only be blocks",
                syntax::error::ILLEGAL_LABEL_STATEMENT,
                std::pair{0UZ, 3UZ}};
    };

    helpers::test_parser_fail("a: defer 3;", expected_diag());
    helpers::test_parser_fail("a: return 3;", expected_diag());
}

TEST_CASE("Illegal implicit access operand") {
    helpers::test_parser_fail(
        ".a::b",
        syntax::diagnostic{"Module access expressions must have outer accessors or identifiers",
                           syntax::error::ILLEGAL_OUTER_ACCESSOR_TYPE,
                           std::pair{0UZ, 0UZ}});
}

TEST_CASE("Prefix without operand") {
    helpers::test_parser_fail(".",
                              syntax::diagnostic{"Prefix expressions require an operand",
                                                 syntax::error::PREFIX_MISSING_OPERAND,
                                                 std::pair{0UZ, 0UZ}});

    helpers::test_parser_fail("!;",
                              syntax::diagnostic{"No prefix parse function for SEMICOLON(;) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 1UZ}});
}

TEST_CASE("Missing inner scope of resolution expression") {
    helpers::test_parser_fail(
        "A:: ;",
        syntax::diagnostic{
            "Expected token IDENT, found SEMICOLON", syntax::error::UNEXPECTED_TOKEN, 0, 4});
}

TEST_CASE("Illegal inner scope of resolution expression") {
    helpers::test_parser_fail(
        "A::2;",
        syntax::diagnostic{
            "Expected token IDENT, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 3});
}

TEST_CASE("Illegal outer scope of resolution expression") {
    helpers::test_parser_fail(
        "2::A;",
        syntax::diagnostic{"Module access expressions must have outer accessors or identifiers",
                           syntax::error::ILLEGAL_OUTER_ACCESSOR_TYPE,
                           std::pair{0UZ, 0UZ}});
}

TEST_CASE("Missing inner member of dot expression") {
    helpers::test_parser_fail(
        "A. ;",
        syntax::diagnostic{
            "Expected token IDENT, found SEMICOLON", syntax::error::UNEXPECTED_TOKEN, 0, 3});
}

TEST_CASE("Illegal inner member of dot expression") {
    helpers::test_parser_fail(
        "A.2;",
        syntax::diagnostic{
            "Expected token IDENT, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 2});
}

} // namespace ghoti::tests
