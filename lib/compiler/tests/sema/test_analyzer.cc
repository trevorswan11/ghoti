#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;
namespace syms  = sema::symbols;
namespace types = sema::types;

namespace {

constexpr std::string_view main_gh{R"(
import std;

pub const main := fn(args: [][:0]u8): i32 {
    const message := "Hello, world!";
    std::io::println(message);
    return 0;
};
)"};

constexpr std::string_view std_gh{R"(
pub import "io.gh" as io;
)"};

constexpr std::string_view io_gh{R"(
pub const println := fn(str: []u8): void {};
)"};

// The table index should point to the table where the module was first declared
[[nodiscard]] auto check_inner_module(helpers::sema_test_context& ctx,
                                      mod::module&                enclosing_module,
                                      std::string_view            name,
                                      usize                       table_idx,
                                      bool                        is_public)
    -> std::pair<mod::module&, const sema::type&> {
    const auto [sym, sym_data, node_data, type, type_data]{
        ctx.get_full_sym_info<syms::node_t, ast::import_stmt, types::module>(
            name, table_idx, enclosing_module)};

    CHECK(sym.get_kind_opt() == sema::symbol_kind::MODULE);
    CHECK(sym.is_public(enclosing_module) == is_public);
    REQUIRE(node_data.get_name(enclosing_module.ast).second == name);
    auto& module{type_data.imported};
    CHECK(module.is_ok());
    return {module, type};
}

} // namespace

TEST_CASE("Full sema pipeline") {
    auto ctx{
        helpers::analyze_and_check("main.gh",
                                   main_gh,
                                   mock_file{.path = "std.gh", .source = std_gh, .name = "std"},
                                   mock_file{.path = "io.gh", .source = io_gh})};
    ctx->verify_registry_resolved();

    REQUIRE(ctx->analyzer.get_registry().size() == 22);
    auto& root_module{ctx->root_mod};
    CHECK(root_module.root_table_idx == 0);
    auto [std_module, std_module_type]{check_inner_module(*ctx, root_module, "std", 0, false)};
    CHECK(std_module.root_table_idx == 1);
    auto [io_module, io_module_type]{check_inner_module(*ctx, std_module, "io", 1, true)};
    CHECK(io_module.root_table_idx == 2);

    SECTION("Main function validation") {
        const auto [main_sym, main_sym_data, main_node_data, main_type, main_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, types::function>("main", 0)};

        CHECK(main_sym.is_public(root_module));
        CHECK(main_sym.get_kind_opt() == sema::symbol_kind::CALLABLE);
        const auto& fn_expr{
            UNWRAP(root_module.ast.get_as_opt<ast::function_expr>(*main_node_data.value))};

        const auto fn_idx{UNWRAP(main_type.get_symbol_table_idx_opt(), 4UZ)};
        CHECK(main_type == ctx->get_type(sema::type_kind::FUNCTION, fn_idx));
        CHECK(main_type_data.params.size() == 1);

        // Verify the parameter type
        const auto& u8_type{ctx->get_type(sema::type_kind::U8)};
        const auto& u8_slice_type{ctx->get_type(sema::type_kind::SLICE, true, u8_type)};
        const auto& u8_slice_slice_type =
            ctx->get_type(sema::type_kind::SLICE, false, u8_slice_type);

        // Verify the parameter type & symbol
        {
            const auto [param_sym, param_sym_data, param_type]{
                ctx->get_type_sym_info<syms::parameter>(
                    "args", fn_idx, root_module, &syms::parameter::name)};
            CHECK(param_sym.get_kind_opt() == sema::symbol_kind::VALUE);
            CHECK(param_type == u8_slice_slice_type);

            const auto& args_param_explicit_type =
                UNWRAP(root_module.get_sema_type_opt(param_sym_data.explicit_type));
            CHECK(args_param_explicit_type == u8_slice_slice_type);
            CHECK(*main_type_data.params[0] == u8_slice_slice_type);
        }

        // Verify the return type
        {
            const auto& return_type{main_type_data.return_type};
            CHECK(return_type == ctx->get_type(sema::type_kind::I32));
        }

        // Verify the function body
        {
            const auto [msg_sym, msg_sym_data, msg_node_data, msg_type]{
                ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("message", fn_idx)};

            CHECK_FALSE(msg_sym.is_public(root_module));
            CHECK(msg_sym.get_kind_opt() == sema::symbol_kind::VALUE);

            const auto msg_size{ctx->get_string_literal_size(*msg_node_data.value)};
            CHECK(msg_type == ctx->get_type(sema::type_kind::ARRAY, true, msg_size, u8_type));

            const auto& call_expr{helpers::lookup_expression<ast::call_expr>(
                UNWRAP(root_module.ast.get_as_opt<ast::block_stmt>(fn_expr.body)), root_module)};

            CHECK(call_expr.arguments.size() == 1);
            const auto arg{UNWRAP(call_expr.arguments[0].as_opt<ast::expr_handle>())};
            auto&      arg_type{UNWRAP(ctx->root_mod.get_sema_type_opt(arg))};
            CHECK(arg_type == msg_type);

            const auto& access_expr{
                UNWRAP(root_module.ast.get_as_opt<ast::module_access_expr>(call_expr.function))};
            const auto& println_fn_type = UNWRAP(root_module.get_sema_type_opt(access_expr.inner));
            CHECK(println_fn_type == ctx->get_type(sema::type_kind::FUNCTION, 3));

            // The outer part of resolution should be two modules
            const auto& access_outer{
                UNWRAP(root_module.ast.get_as_opt<ast::module_access_expr>(access_expr.outer))};
            const auto& access_std_expr = UNWRAP(root_module.get_sema_type_opt(access_outer.outer));
            CHECK(access_std_expr == std_module_type);

            const auto& access_io_expr = UNWRAP(root_module.get_sema_type_opt(access_outer.inner));
            CHECK(access_io_expr == io_module_type);
        }
    }

    SECTION("Println function validation") {
        const auto [println_sym,
                    println_sym_data,
                    println_node_data,
                    println_type,
                    println_type_data] =
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, types::function>(
                "println", *io_module.root_table_idx, io_module);
        CHECK(println_sym.is_public(io_module));
        CHECK(println_sym.get_kind_opt() == sema::symbol_kind::CALLABLE);

        const auto fn_idx{UNWRAP(println_type.get_symbol_table_idx_opt(), 3UZ)};
        CHECK(println_type == ctx->get_type(sema::type_kind::FUNCTION, fn_idx));
        CHECK(println_type_data.params.size() == 1);

        // Verify the parameter type
        const auto& u8_slice_type =
            ctx->get_type(sema::type_kind::SLICE, false, ctx->get_type(sema::type_kind::U8));

        // Verify the parameter type & symbol
        {
            const auto [param_sym, param_sym_data, param_type]{
                ctx->get_type_sym_info<syms::parameter>(
                    "str", fn_idx, io_module, &syms::parameter::name)};
            CHECK(param_sym.get_kind_opt() == sema::symbol_kind::VALUE);
            CHECK(param_type == u8_slice_type);

            const auto& str_param_explicit_type =
                UNWRAP(io_module.get_sema_type_opt(param_sym_data.explicit_type));
            CHECK(str_param_explicit_type == u8_slice_type);
            CHECK(*println_type_data.params[0] == u8_slice_type);
        }

        // Verify the return type
        {
            const auto& return_type{println_type_data.return_type};
            const auto& void_type{ctx->get_type(sema::type_kind::VOID_)};
            CHECK(return_type == void_type);
        }
    }
}

} // namespace ghoti::tests
