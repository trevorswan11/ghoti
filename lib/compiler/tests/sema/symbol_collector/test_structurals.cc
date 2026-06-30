#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <gsl/pointers>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

auto test_user_type(std::string_view input, sema::type_kind kind, usize expected_reg_count)
    -> stdx::box<helpers::sema_test_context> {
    auto [ctx, idx]{helpers::collect_and_check(input)};
    const auto& registry{ctx->analyzer.get_registry()};
    REQUIRE(registry.size() == expected_reg_count);

    const auto [sym, sym_data, node_data]{
        ctx->get_ast_sym_info<sema::symbols::node_t, ast::decl_stmt>("a", idx)};
    CHECK(sym.get_kind_opt() == sema::symbol_kind::TYPE);

    auto&      actual_type = UNWRAP(ctx->root_mod.get_sema_type_opt(UNWRAP(node_data.value)));
    const auto type_idx{UNWRAP(actual_type.get_symbol_table_idx_opt(), idx + 1)};
    CHECK(actual_type == ctx->get_type(kind, type_idx));

    const auto& value_type{UNWRAP(ctx->root_mod.get_sema_type_opt(*node_data.value))};
    CHECK(actual_type == value_type);
    return std::move(ctx);
}

} // namespace

TEST_CASE("Struct hollow types") {
    auto        ctx{test_user_type(
        "const a := struct { b: i32, var foo := bar; };", sema::type_kind::STRUCT, 2)};
    const auto& registry{ctx->analyzer.get_registry()};
    const auto& field{UNWRAP(registry.get_from_opt(1, "b"))};
    REQUIRE(field.get_data().as_opt<sema::symbols::struct_field>());
    ctx->test_common_decl_collection(1);
}

TEST_CASE("Enum hollow types") {
    auto ctx{test_user_type("const a := enum {b, const foo := bar; };", sema::type_kind::ENUM, 2)};
    const auto& registry{ctx->analyzer.get_registry()};
    const auto& enumeration{UNWRAP(registry.get_from_opt(1, "b"))};
    REQUIRE(enumeration.get_data().as_opt<sema::symbols::enumeration>());
    ctx->test_common_decl_collection(1);
}

TEST_CASE("Union hollow types") {
    auto        ctx{test_user_type(
        "const a := union { b: i32, const foo := bar; };", sema::type_kind::UNION, 2)};
    const auto& registry{ctx->analyzer.get_registry()};
    const auto& field{UNWRAP(registry.get_from_opt(1, "b"))};
    REQUIRE(field.get_data().as_opt<sema::symbols::union_field>());
    ctx->test_common_decl_collection(1);
}

TEST_CASE("Public using query") {
    auto [ctx, idx]{helpers::collect_and_check("pub using I = i32;")};
    const auto& registry{ctx->analyzer.get_registry()};
    const auto& int_alias{UNWRAP(registry.get_from_opt(idx, "I"))};
    CHECK(int_alias.get_kind_opt() == sema::symbol_kind::TYPE);
    CHECK(int_alias.is_public(ctx->root_mod));
}

TEST_CASE("Shadowing member/field declarations") {
    const auto expected_diag = [](usize col) -> sema::diagnostic {
        return {"Attempt to shadow identifier 'a'; previous declaration here: 1:1",
                sema::error::SHADOWING_DECLARATION,
                std::pair{0UZ, col}};
    };

    helpers::test_collector_fail("const a := struct { var a := 2; };", expected_diag(20));
    helpers::test_collector_fail("const a := struct { a: i32, var b := 2; };", expected_diag(20));
    helpers::test_collector_fail("const a := enum {a};", expected_diag(17));
    helpers::test_collector_fail("const a := enum {b const a := 2; };", expected_diag(19));
    helpers::test_collector_fail("const a := union { a: i32 };", expected_diag(19));
    helpers::test_collector_fail("const a := union { b: i32 const a := 2; };", expected_diag(26));
}

} // namespace ghoti::tests
