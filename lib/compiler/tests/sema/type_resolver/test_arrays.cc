#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Array resolution with explicit type") {
    auto [ctx, idx]{helpers::resolve_and_check("const a: [2]i32 = [2]i32{1, 2, };")};
    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    const auto& array{
        UNWRAP(ctx->root_mod.ast.get_as_opt<ast::explicit_array_type>(*node_data.explicit_type))};

    // The explicit type in the decl can't have a true size at this point
    const auto& array_type{ctx->get_type(sema::type_kind::TYPE, &array)};
    CHECK(type == array_type);
    const auto& ident_type{UNWRAP(ctx->root_mod.get_sema_type_opt(node_data.name))};
    CHECK(ident_type == array_type);
    const auto& et_type = UNWRAP(ctx->root_mod.get_sema_type_opt(*node_data.explicit_type));
    CHECK(et_type == array_type);

    // The actual value type is properly typed
    const auto& item_type{ctx->get_type(sema::type_kind::I32)};
    const auto& array_literal_type{ctx->get_type(sema::type_kind::ARRAY, false, 2, item_type)};
    const auto& value_type{UNWRAP(ctx->root_mod.get_sema_type_opt(*node_data.value))};
    CHECK(value_type == array_literal_type);

    const auto& type_data = UNWRAP(array_literal_type.get_data().as_opt<sema::types::array>());
    CHECK(type_data.underlying == item_type);
    CHECK(type_data.len == 2);
    CHECK_FALSE(type_data.null_terminated);

    // This is not generally true but happens to be here
    for (const auto& arr = UNWRAP(ctx->root_mod.ast.get_as_opt<ast::array_expr>(*node_data.value));
         const auto  item : arr.items) {
        CHECK(item_type == ctx->root_mod.get_sema_type(item));
    }
}

TEST_CASE("Array resolution with implicit type") {
    auto [ctx, idx]{helpers::resolve_and_check("const a := [4]u64{1, 2, 3, 4 };")};
    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};

    const auto& item_type{ctx->get_type(sema::type_kind::U64)};
    const auto& array_literal_type{ctx->get_type(sema::type_kind::ARRAY, false, 4, item_type)};
    CHECK(type == array_literal_type);
    const auto& ident_type{UNWRAP(ctx->root_mod.get_sema_type_opt(node_data.name))};
    CHECK(ident_type == array_literal_type);
    const auto& value_type{UNWRAP(ctx->root_mod.get_sema_type_opt(*node_data.value))};
    CHECK(value_type == array_literal_type);
}

TEST_CASE("Indexing with single accessors") {
    const auto test_index = [](std::string_view type_mod) -> void {
        auto [ctx, idx]{
            helpers::resolve_and_check(fmt::format("var a: {}u32; const b := a[0];", type_mod))};
        const auto [sym, sym_data, type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};
        CHECK(type == ctx->get_type(sema::type_kind::U32));
    };

    test_index("[]");
    test_index("[3]");
    test_index("^");
}

TEST_CASE("Indexing with slice accessor") {
    auto [ctx, idx]{helpers::resolve_and_check("var a: ^u32; const b := a[0..4];")};
    const auto [sym, sym_data, type]{ctx->get_type_sym_info<syms::node_t>("b", idx)};

    const auto& i32_type{ctx->get_type(sema::type_kind::U32)};
    const auto& slice_type{ctx->get_type(sema::type_kind::SLICE, false, i32_type)};
    CHECK(type == slice_type);
}

TEST_CASE("Well-formed arrays with structural types") {
    helpers::resolve_and_check("const A := struct { a: i32, const b := [_]A{}; };");
    helpers::resolve_and_check("const A := struct { a: []A, };");
    helpers::resolve_and_check(
        "const A := struct { a: [6]&@this(), const b := fn(c: ^@this()): i32 {}; };");
    helpers::resolve_and_check("const A := union { a: [4]^@this(), };");
    helpers::resolve_and_check("const A := union { a: []@this(), };");
}

TEST_CASE("Illegal index target") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        "var a: u32; const b := a[0];",
        sema::diagnostic{"Can only index slices, arrays, and pointers; found 'u32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 23UZ}})};
    ctx->check_poisoned<syms::node_t>("b", idx);
}

TEST_CASE("Illegal arrays dependent on incomplete types") {
    const auto expected_diag = [](usize col) -> sema::diagnostic {
        return {"Array elements cannot have an incomplete type",
                sema::error::CYCLIC_DEPENDENCY,
                std::pair{0UZ, col}};
    };

    helpers::test_resolver_fail("const A := struct { a: [3]A, };", expected_diag(26));
    helpers::test_resolver_fail("const A := struct { a: [3]@this(), };", expected_diag(26));
    helpers::test_resolver_fail("const A := struct { a: @typeOf([3]A), };", expected_diag(34));
    helpers::test_resolver_fail("const A := union { a: [1]A, };", expected_diag(25));
    helpers::test_resolver_fail("const A := union { a: @typeOf([1]A), };", expected_diag(33));
    helpers::test_resolver_fail("const A := struct { a: auto = [_]A{}, };", expected_diag(33));
}

} // namespace ghoti::tests
