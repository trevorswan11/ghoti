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
        return {"A function type is immutable; use `&fn`/`^fn` without `mut`",
                syntax::error::ILLEGAL_FUNCTION_TYPE_MODIFIER,
                std::pair{0UZ, 11UZ}};
    };

    const auto illegal{GENERATE("let mut a: &mut fn(): void;"sv,
                                "let mut a: ^mut fn(): void;"sv,
                                "let mut a: &mut extern fn(): void;"sv,
                                "let mut a: &mut dyn Fn(): void;"sv)};
    helpers::test_parser_fail(illegal, expected_diag());
}

TEST_CASE("An `extern fn` type literal cannot carry a body") {
    helpers::test_parser_fail(
        "const f = extern fn(): void { };",
        syntax::diagnostic{"`extern fn(...)` names a function pointer type and cannot have a body",
                           syntax::error::EXPLICIT_FN_TYPE_HAS_BODY,
                           std::pair{0UZ, 17UZ}});
}

TEST_CASE("Bodied function type") {
    helpers::test_parser_fail("let mut a: ^mut fn(): void { b; };",
                              syntax::diagnostic{"Function types may not have a body",
                                                 syntax::error::EXPLICIT_FN_TYPE_HAS_BODY,
                                                 std::pair{0UZ, 16UZ}});
}

TEST_CASE("Function return type restrictions") {
    helpers::test_parser_fail("let mut a: fn(): &void;",
                              syntax::diagnostic{"Explicit `void` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_VOID_TYPE_MODIFIER,
                                                 std::pair{0UZ, 17UZ}});

    helpers::test_parser_fail("let mut a: fn(): &type;",
                              syntax::diagnostic{"Explicit `type` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_TYPE_TYPE_MODIFIER,
                                                 std::pair{0UZ, 17UZ}});

    helpers::test_parser_fail("let mut a: fn(): &noreturn;",
                              syntax::diagnostic{"Explicit `noreturn` type cannot have a modifier",
                                                 syntax::error::ILLEGAL_NORETURN_TYPE_MODIFIER,
                                                 std::pair{0UZ, 17UZ}});
}

TEST_CASE("mut on a type without a pointer, reference, or slice") {
    const syntax::diagnostic bare{
        "`mut` only qualifies what a `^`, `&`, or `[]` reaches; a value's "
        "mutability comes from `let mut`",
        syntax::error::ILLEGAL_VALUE_MUT_MODIFIER,
        std::pair{0UZ, 11UZ}};
    helpers::test_parser_fail("let mut a: mut i32 = 1;", syntax::diagnostic{bare});
    helpers::test_parser_fail("let mut a: mut mut i32 = 1;", syntax::diagnostic{bare});
    helpers::test_parser_fail(
        "let mut a: mut volatile i32 = 1;",
        syntax::diagnostic{"`mut volatile T` is just `volatile T`: a value's mutability comes from "
                           "`let mut`, not its type",
                           syntax::error::ILLEGAL_VALUE_MUT_MODIFIER,
                           std::pair{0UZ, 11UZ}});
    helpers::test_parser_fail(
        "let mut a: []mut mut volatile i32 = undefined;",
        syntax::diagnostic{"`mut volatile T` is just `volatile T`: a value's mutability comes from "
                           "`let mut`, not its type",
                           syntax::error::ILLEGAL_VALUE_MUT_MODIFIER,
                           std::pair{0UZ, 17UZ}});
}

} // namespace ghoti::tests
