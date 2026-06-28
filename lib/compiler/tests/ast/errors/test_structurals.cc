#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Empty enum") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Enums must be declared with at least one enumeration",
                syntax::error::EMPTY_ENUM,
                std::pair{0UZ, 0UZ}};
    };

    helpers::test_parser_fail("enum {};", expected_diag());
    helpers::test_parser_fail("enum : T {};", expected_diag());
}

TEST_CASE("Illegal underlying type") {
    helpers::test_parser_fail("enum : 4 {A};",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 7});
    helpers::test_parser_fail(R"(enum : "e" {A};)",
                              syntax::diagnostic{syntax::error::ILLEGAL_IDENTIFIER, 0, 7});
}

TEST_CASE("Empty enum with decl") {
    helpers::test_parser_fail(
        "enum : i64 { const b := fn(&self, a: A): C { c; }; };",
        syntax::diagnostic{"Enums must be declared with at least one enumeration",
                           syntax::error::EMPTY_ENUM,
                           std::pair{0UZ, 0UZ}});
}

TEST_CASE("Out of order enum") {
    helpers::test_parser_fail("enum : i64 { A = 2l const b := fn(&self, a: A): C { c; }; B = 2l };",
                              syntax::diagnostic{"Expected token SEMICOLON, found RBRACE",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 65UZ}});
}

TEST_CASE("Illegal struct members") {
    helpers::test_parser_fail(
        "struct { extern var foo: bar; };",
        syntax::diagnostic{"Member declarations may neither be marked extern nor export",
                           syntax::error::INVALID_MEMBER,
                           std::pair{0UZ, 9UZ}});

    helpers::test_parser_fail("struct { defer {}; };",
                              syntax::diagnostic{"Expected token IDENT, found DEFER",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 9UZ}},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 19UZ}});
}

TEST_CASE("Illegal union field name") {
    helpers::test_parser_fail(
        "union { 2: i32 };",
        syntax::diagnostic{
            "Expected token IDENT, found INT_10", syntax::error::UNEXPECTED_TOKEN, 0, 8});
}

TEST_CASE("Illegal union field type") {
    helpers::test_parser_fail("union { a: 2 };",
                              syntax::diagnostic{syntax::error::ILLEGAL_EXPLICIT_TYPE, 0, 9});
}

TEST_CASE("Empty union") {
    helpers::test_parser_fail("union { };",
                              syntax::diagnostic{"Unions must be declared with at least one field",
                                                 syntax::error::EMPTY_UNION,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("Empty union with decl") {
    helpers::test_parser_fail("union { const b := fn(&self, a: A): C { c; }; };",
                              syntax::diagnostic{"Unions must be declared with at least one field",
                                                 syntax::error::EMPTY_UNION,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("Out of order union") {
    helpers::test_parser_fail("union { a: i32, const b := fn(&self, a: A): C { c; }; b: i32, };",
                              syntax::diagnostic{"Expected token SEMICOLON, found COMMA",
                                                 syntax::error::UNEXPECTED_TOKEN,
                                                 std::pair{0UZ, 60UZ}});
}

TEST_CASE("Illegal type aliasing/definition") {
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"User-defined types can only be defined with non-modified aliases",
                syntax::error::ILLEGAL_USING_ALIAS_WITH_MODIFIERS,
                std::pair{0UZ, 10UZ}};
    };

    helpers::test_parser_fail("using U = &union { a: i32 };", expected_diag());
    helpers::test_parser_fail("using S = ^struct { pub var foo := bar; };", expected_diag());
    helpers::test_parser_fail("using E = ^enum { a };", expected_diag());
}

} // namespace ghoti::tests
