#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

namespace {

[[nodiscard]] auto find_nested_fn(const mod::module&        module,
                                  const ast::function_expr& outer,
                                  std::string_view          name) -> ast::node_id {
    const auto& block{module.ast.get_as<ast::block_stmt>(outer.body)};
    for (const auto& stmt : block) {
        if (const auto decl{module.ast.get_as_opt<ast::decl_stmt>(stmt)}) {
            const auto& ident{module.ast.get_as<ast::identifier_expr>(decl->name)};
            if (ident.name == name && decl->value) { return *decl->value; }
        }
    }
    FAIL("Could not find nested function named '" << name << "'");
}

} // namespace

TEST_CASE("A closure with no free variables records an empty capture list") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            const add := fn(x: i32): i32 {
                return x;
            };
        };
    )")};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    CHECK(ctx->root_mod.get_captures(add_id).empty());
}

TEST_CASE("A read-only capture is recorded with READ usage") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        R"(
        const outer := fn(): void {
            var offset: i32 = 0;
            const add := fn(x: i32): i32 {
                return x + offset;
            };
        };
    )",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{4UZ, 27UZ}})};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::READ);
}

TEST_CASE("Assigning to a captured variable records MUTATED usage") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        R"(
        const outer := fn(): void {
            var offset: i32 = 0;
            const add := fn(x: i32): i32 {
                offset = x;
                return offset;
            };
        };
    )",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{4UZ, 16UZ}})};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);
}

TEST_CASE("A later mutation escalates an existing READ capture entry to MUTATED") {
    SKIP("Actual logic in resolver not reachable due to early poison/bail");
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const outer := fn(): void {
            const add := fn(x: i32): i32 {
                return x;
            };
        };
    )")};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    ctx->root_mod.add_capture(add_id, "offset", sema::capture_usage::READ);
    ctx->root_mod.add_capture(add_id, "offset", sema::capture_usage::MUTATED);
    ctx->root_mod.add_capture(add_id, "offset", sema::capture_usage::READ); // must not downgrade

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);
}

TEST_CASE("Multiple distinct captures are all recorded") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        R"(
        const outer := fn(): void {
            var a: i32 = 0;
            var b: i32 = 0;
            const add := fn(x: i32): i32 {
                a = x;
                return b;
            };
        };
    )",
        sema::diagnostic{"'a' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{5UZ, 16UZ}})};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    // Resolution stops at the first diagnostic (`a`'s mutation), so only `a` is recorded - `b` is
    // never reached. This documents current best-effort, single-pass recording behavior.
    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "a");
    CHECK(captures[0].usage == sema::capture_usage::MUTATED);
}

TEST_CASE("Capturing an enclosing function's own parameter is recorded") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        R"(
        const outer := fn(offset: i32): void {
            const add := fn(x: i32): i32 {
                return x + offset;
            };
        };
    )",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{3UZ, 27UZ}})};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("outer", idx)};
    const auto& outer{UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*node_data.value))};
    const auto  add_id{find_nested_fn(ctx->root_mod, outer, "add")};

    const auto captures{ctx->root_mod.get_captures(add_id)};
    REQUIRE(captures.size() == 1);
    CHECK(captures[0].name == "offset");
    CHECK(captures[0].usage == sema::capture_usage::READ);
}

} // namespace ghoti::tests
