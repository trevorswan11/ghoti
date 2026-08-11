#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

namespace {

[[nodiscard]] auto get_outer_fn(const auto& ctx, usize idx, std::string_view name)
    -> const ast::function_expr& {
    const auto [sym, sym_data, node_data, type]{
        ctx->template get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
    return UNWRAP(ctx->root_mod.ast.template get_as_opt<ast::function_expr>(*node_data.value));
}

} // namespace

TEST_CASE("A closure with no free variables records an empty capture list and stays FUNCTION") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            const add := fn(x: i32): i32 {
                return x;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    CHECK(ctx->root_mod.get_captures(add_id).empty());
    CHECK(ctx->root_mod.get_sema_type(add_id).get_kind() == sema::type_kind::FUNCTION);
}

TEST_CASE("A read-only primitive capture is recorded as READ and inferred VALUE") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var offset: i32 = 0;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::READ);

    const auto& closure_type{ctx->root_mod.get_sema_type(add_id)};
    CHECK(closure_type.get_kind() == sema::type_kind::CLOSURE);
    const auto& cl{UNWRAP(closure_type.get_data().as_opt<sema::types::closure_t>())};
    REQUIRE(cl.captures.size() == 1);
    CHECK(cl.captures[0].name == "offset");
    CHECK(cl.captures[0].mode == sema::types::capture_mode::VALUE);
    CHECK(cl.captures[0].captured_type->get_kind() == sema::type_kind::I32);
}

TEST_CASE("Assigning to a captured variable records MUTATED usage and infers MUT_REF") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var offset: i32 = 0;
            const add := fn(x: i32): i32 {
                offset = x;
                return offset;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);

    const auto& closure_type{ctx->root_mod.get_sema_type(add_id)};
    const auto& cl{UNWRAP(closure_type.get_data().as_opt<sema::types::closure_t>())};
    REQUIRE(cl.captures.size() == 1);
    CHECK(cl.captures[0].mode == sema::types::capture_mode::MUT_REF);
}

TEST_CASE("A read before a later mutation still escalates to MUTATED") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var offset: i32 = 0;
            const add := fn(x: i32): i32 {
                const before := offset;
                offset = x;
                return before;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);
}

TEST_CASE("Multiple distinct captures are all recorded") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var a: i32 = 0;
            var b: i32 = 0;
            const add := fn(x: i32): i32 {
                a = x;
                return b;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 2);
    CHECK(captures[0].name == "a");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);
    CHECK(captures[1].name == "b");
    CHECK(captures[1].usage == sema::capture_usage::READ);
}

TEST_CASE("A read-only aggregate capture is inferred REF, not VALUE") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            var arr: [3uz]i32 = [_]i32{1, 2, 3};
            const add := fn(): i32 {
                return arr[0];
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto& closure_type{ctx->root_mod.get_sema_type(add_id)};
    const auto& cl{UNWRAP(closure_type.get_data().as_opt<sema::types::closure_t>())};
    REQUIRE(cl.captures.size() == 1);
    CHECK(cl.captures[0].name == "arr");
    CHECK(cl.captures[0].mode == sema::types::capture_mode::REF);
}

TEST_CASE("Capturing an enclosing function's own parameter is recorded") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(offset: i32): void {
            const add := fn(x: i32): i32 {
                return x + offset;
            };
        };
    )")};

    const auto& outer{get_outer_fn(ctx, idx, "outer")};
    const auto  add_id{helpers::find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::READ);
}

} // namespace ghoti::tests
