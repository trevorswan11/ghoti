#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

TEST_CASE("Function type restrictions") {
    using namespace std::string_view_literals;
    const auto expected_diag = [] -> syntax::diagnostic {
        return {"Functions types may only be values or pointers",
                syntax::error::ILLEGAL_FUNCTION_TYPE_MODIFIER,
                std::pair{0UZ, 7UZ}};
    };

    const auto illegal{GENERATE("var a: &fn(): void;"sv, "var a: &mut fn(): void;"sv)};
    helpers::test_parser_fail(illegal, expected_diag());
}

TEST_CASE("Bodied function type") {
    helpers::test_parser_fail("var a: ^mut fn(): void { b; };",
                              syntax::diagnostic{"Function types may not have a body",
                                                 syntax::error::EXPLICIT_FN_TYPE_HAS_BODY,
                                                 std::pair{0UZ, 12UZ}},
                              syntax::diagnostic{"No prefix parse function for RBRACE(}) found",
                                                 syntax::error::MISSING_PREFIX_PARSER,
                                                 std::pair{0UZ, 28UZ}});
}

TEST_CASE("Function return type restrictions") {
    helpers::test_parser_fail("var a: fn(): &void;",
                              syntax::diagnostic{"Explicit `void` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_VOID_TYPE_MODIFIER,
                                                 std::pair{0UZ, 13UZ}});

    helpers::test_parser_fail("var a: fn(): &type;",
                              syntax::diagnostic{"Explicit `type` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_TYPE_TYPE_MODIFIER,
                                                 std::pair{0UZ, 13UZ}});

    helpers::test_parser_fail("var a: fn(): &noreturn;",
                              syntax::diagnostic{"Explicit `noreturn` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_NORETURN_TYPE_MODIFIER,
                                                 std::pair{0UZ, 13UZ}});
}

} // namespace ghoti::tests
