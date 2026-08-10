#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Function declaration and call type resolution") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const a := fn(b: i32, c: ^@typeOf(b), d: [:0]u8): bool {
            return true;
        };

        const int: i32 = 12;
        const result := a(1, ^int, "Hello, World");
    )")};

    const auto& i32_type{ctx->get_type(sema::type_kind::I32)};
    const auto& bool_type{ctx->get_type(sema::type_kind::BOOL)};

    const auto [a_decl_sym, a_decl_sym_data, a_decl_node_data, a_decl_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};

    SECTION("Function validation") {
        CHECK(a_decl_sym.get_kind_opt() == sema::symbol_kind::CALLABLE);

        const auto check_param_type = [&](std::string_view  name,
                                          const sema::type& expected_type) -> void {
            const auto [sym, sym_data, type]{ctx->get_type_sym_info<syms::parameter>(
                name, 1, stdx::none, &syms::parameter::name)};
            CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
            CHECK(type == expected_type);
        };

        check_param_type("b", i32_type);

        const auto& c_meta{ctx->get_type(sema::type_kind::TYPE, i32_type)};
        const auto& c_ptr{ctx->get_type(sema::type_kind::POINTER, c_meta)};
        check_param_type("c", c_ptr);
        const auto& c_meta_data = UNWRAP(c_meta.get_data().as_opt<sema::types::meta_type>());
        CHECK(c_meta_data.instance == i32_type);

        const auto& u8_slice =
            ctx->get_type(sema::type_kind::SLICE, true, ctx->get_type(sema::type_kind::U8));
        check_param_type("d", u8_slice);

        const auto& fn{
            UNWRAP(ctx->root_mod.ast.get_as_opt<ast::function_expr>(*a_decl_node_data.value))};
        const auto& return_type{ctx->root_mod.get_sema_type(fn.explicit_return_type)};
        CHECK(return_type == bool_type);
    }

    SECTION("Call validation") {
        const auto [decl_sym, decl_sym_data, decl_node_data, decl_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("result", idx)};
        CHECK(decl_sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(decl_type == bool_type);

        const auto& call{
            UNWRAP(ctx->root_mod.ast.get_as_opt<ast::call_expr>(*decl_node_data.value))};
        CHECK(call.arguments.size() == 3);
        const auto& call_type{UNWRAP(ctx->root_mod.get_sema_type_opt(call.function))};
        CHECK(a_decl_type == call_type);

        const auto check_arg_type = [&](usize arg_idx, const sema::type& expected_type) -> void {
            REQUIRE(arg_idx < call.arguments.size());
            const auto  arg{UNWRAP(call.arguments[arg_idx].as_opt<ast::expr_handle>())};
            const auto& arg_type{UNWRAP(ctx->root_mod.get_sema_type_opt(arg))};
            CHECK(expected_type == arg_type);
        };

        check_arg_type(0, i32_type);
        check_arg_type(1, ctx->get_type(sema::type_kind::POINTER, i32_type));

        const auto last_arg{UNWRAP(call.arguments[2].as_opt<ast::expr_handle>())};
        const auto str_size{ctx->get_string_literal_size(last_arg)};

        const auto& u8_type{ctx->get_type(sema::type_kind::U8)};
        const auto& null_string_type =
            ctx->get_type(sema::type_kind::ARRAY, true, str_size, u8_type);

        const auto& last_arg_type{UNWRAP(ctx->root_mod.get_sema_type_opt(last_arg))};
        CHECK(null_string_type == last_arg_type);
    }
}

TEST_CASE("Self parameters in structural types") {
    // Assumes the type has a member function called foo that takes self by reference
    const auto check_structural_type = [](std::string_view input,
                                          sema::type_kind  self_kind) -> void {
        auto [ctx, idx]{helpers::resolve_and_check(input)};

        const auto [a_decl_sym, a_decl_sym_data, a_decl_node_data, a_decl_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
        CHECK(a_decl_sym.get_kind_opt() == sema::symbol_kind::TYPE);
        const auto struct_idx{UNWRAP(a_decl_type.get_symbol_table_idx_opt(), 1UZ)};

        const auto [fn_sym, fn_sym_data, fn_node_data, fn_type, fn_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>(
                "foo", struct_idx)};
        REQUIRE(fn_type_data.params.size() == 1);
        const auto& expected_param_type{ctx->get_type(self_kind, struct_idx, a_decl_type)};
        CHECK(expected_param_type == *fn_type_data.params[0]);
    };

    check_structural_type(R"(const a := struct {
        const foo := fn(&self): void {};
    };)",
                          sema::type_kind::REFERENCE);

    check_structural_type(R"(const a := enum {
        b,
        const foo := fn(&self): void {};
    };)",
                          sema::type_kind::REFERENCE);

    check_structural_type(R"(const a := union {
        b: i32,
        const foo := fn(^self): void {};
    };)",
                          sema::type_kind::POINTER);
}

TEST_CASE("Deferred return type from user function") {
    auto [ctx, idx]{helpers::resolve_and_check("const a := fn(): type {}; using B = a();")};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::using_stmt>("B", idx)};

    const auto& call =
        UNWRAP(ctx->root_mod.ast.get_as_opt<ast::call_expr>(node_data.explicit_type));
    CHECK(type == ctx->get_type(sema::type_kind::TYPE, &call));
    CHECK(&call == &UNWRAP(type.get_data().as_opt<sema::types::deferred_call>()).call);
}

TEST_CASE("Function explicit type resolution") {
    auto [ctx, idx]{helpers::resolve_and_check("var foo: fn(^i32, u32): bool;")};
    const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("foo", idx)};

    const auto& expected_type =
        ctx->get_type(sema::type_kind::FUNCTION,
                      ctx->get_type(sema::type_kind::POINTER, ctx->get_type(sema::type_kind::I32)),
                      ctx->get_type(sema::type_kind::U32),
                      ctx->get_type(sema::type_kind::BOOL));
    CHECK(type == expected_type);
}

TEST_CASE("Function with syntactically ambiguous arguments") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        using a = @typeOf(^i32);
        using b = @typeOf(^mut i32);
        using c = @typeOf(&i32);
        using d = @typeOf(&mut i32);
        using e = @typeOf(^^i32);
        using f = @typeOf(^mut ^i32);
    )")};

    const auto check_ambiguous = [&](std::string_view  name,
                                     const sema::type& instance_type) -> void {
        const auto& meta_type{ctx->get_type(sema::type_kind::TYPE, instance_type)};
        const auto [sym, _, type, data]{
            ctx->get_full_type_sym_info<syms::node_t, sema::types::meta_type>(name, idx)};
        CHECK(type == meta_type);
    };

    namespace mut = sema::types::mut;
    const auto& i32_type{ctx->get_type(sema::type_kind::I32)};
    const auto& i32_const_ptr{ctx->get_type(sema::type_kind::POINTER, i32_type)};

    check_ambiguous("a", i32_const_ptr);
    check_ambiguous("b", ctx->get_type<mut::MUTABLE>(sema::type_kind::POINTER, i32_type));
    check_ambiguous("c", ctx->get_type(sema::type_kind::REFERENCE, i32_type));
    check_ambiguous("d", ctx->get_type<mut::MUTABLE>(sema::type_kind::REFERENCE, i32_type));
    check_ambiguous("e", ctx->get_type(sema::type_kind::POINTER, i32_const_ptr));
    check_ambiguous("f", ctx->get_type<mut::MUTABLE>(sema::type_kind::POINTER, i32_const_ptr));
}

TEST_CASE("Self parameters in non-structural types") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        "const foo := fn(&self): void {};",
        sema::diagnostic{"Self parameters may only be used inside member functions",
                         sema::error::ILLEGAL_SELF_PARAMETER,
                         std::pair{0UZ, 17UZ}})};
    ctx->check_poisoned<syms::node_t>("foo", idx);
}

// TODO: Remove these once captures are supported
TEST_CASE("Implicit capture of an enclosing function's local is rejected") {
    helpers::test_resolver_fail(
        "const outer := fn(): void { var offset: i32 = 0; const add := fn(x: i32): i32 { "
        "return x + offset; }; };",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{0UZ, 91UZ}});

    helpers::test_resolver_fail(
        "const outer := fn(offset: i32): void { const add := fn(x: i32): i32 { "
        "return x + offset; }; };",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{0UZ, 81UZ}});

    helpers::test_resolver_fail(
        "const outer := fn(): void { var offset: i32 = 0; const middle := fn(): void { "
        "const inner := fn(x: i32): i32 { return x + offset; }; }; };",
        sema::diagnostic{"'offset' would require an implicit capture of a variable from an "
                         "enclosing function, which is not yet supported",
                         sema::error::ILLEGAL_IMPLICIT_CAPTURE,
                         std::pair{0UZ, 122UZ}});
}

TEST_CASE("Module-level globals are not implicit captures") {
    helpers::resolve_and_check(R"(
        const GLOBAL: i32 = 0;
        const outer := fn(): void {
            const add := fn(x: i32): i32 {
                return x + GLOBAL;
            };
        };
    )");
}

TEST_CASE("Declared function arity mismatch") {
    auto [ctx, idx]{helpers::test_resolver_fail(
        "const foo := fn(a: i32, b: i32): void {}; const bar := foo(1);",
        sema::diagnostic{
            "Expected 2 arguments, found 1", sema::error::ARITY_MISMATCH, std::pair{0UZ, 55UZ}})};
    ctx->check_poisoned<syms::node_t>("bar", idx);
}

TEST_CASE("Non-callable expression") {
    auto [ctx,
          idx]{helpers::test_resolver_fail("const bar := 5; const foo := bar();",
                                           sema::diagnostic{"Expression is not callable",
                                                            sema::error::NON_CALLABLE_EXPRESSION,
                                                            std::pair{0UZ, 29UZ}})};
    ctx->check_poisoned<syms::node_t>("foo", idx);
}

} // namespace ghoti::tests
