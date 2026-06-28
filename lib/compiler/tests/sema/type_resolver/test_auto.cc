#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Declaration auto type inference") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        var a: auto = 42;
        const b: auto = true;
        var c := 100l;
        const d := false;
    )")};

    const auto& i32_type{ctx->get_type(sema::type_kind::I32)};
    const auto& i64_type{ctx->get_type(sema::type_kind::I64)};
    const auto& bool_type{ctx->get_type(sema::type_kind::BOOL)};

    SECTION("Explicit auto variable declaration adopts value type") {
        const auto [sym, data, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(type == i32_type);
        CHECK(ctx->root_mod.get_sema_type(node.name) == i32_type);
    }

    SECTION("Explicit auto constant declaration adopts value type") {
        const auto [sym, data, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("b", idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(type == bool_type);
        CHECK(ctx->root_mod.get_sema_type(node.name) == bool_type);
    }

    SECTION("Walrus variable declaration adopts value type") {
        const auto [sym, data, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c", idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(type == i64_type);
        CHECK(ctx->root_mod.get_sema_type(node.name) == i64_type);
    }

    SECTION("Walrus constant declaration adopts value type") {
        const auto [sym, data, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("d", idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(type == bool_type);
        CHECK(ctx->root_mod.get_sema_type(node.name) == bool_type);
    }
}

TEST_CASE("Declaration auto without initializer fails") {
    helpers::test_resolver_fail("var a: auto;",
                                sema::diagnostic{"Type 'auto' requires an initializer expression",
                                                 sema::error::AUTO_WITHOUT_INITIALIZER,
                                                 std::pair{0UZ, 0UZ}});
}

TEST_CASE("Illegal auto usage in structural types") {
    SECTION("Struct field without default value") {
        helpers::test_resolver_fail("const S := struct { a: auto, };",
                                    sema::diagnostic{"Struct field 'a' cannot have type 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 23UZ}});
    }

    SECTION("Struct field with default value infers type") {
        auto [ctx, idx]{helpers::resolve_and_check("const S := struct { a: auto = 42, };")};
        const auto [s_sym, s_sym_data, s_node_data, s_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("S", idx)};
        CHECK(s_sym.get_kind_opt() == sema::symbol_kind::TYPE);
        const auto struct_idx{helpers::unwrap(s_type.get_symbol_table_idx_opt(), 1UZ)};

        const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::struct_field>(
            "a", struct_idx, stdx::none, &syms::struct_field::name)};
        CHECK(a_type == ctx->get_type(sema::type_kind::I32));
    }

    SECTION("Union field cannot have auto") {
        helpers::test_resolver_fail("const U := union { a: auto, };",
                                    sema::diagnostic{"Union field 'a' cannot have type 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 22UZ}});
    }
}

TEST_CASE("Illegal auto usage in type aliases and function types") {
    SECTION("Type alias cannot be auto") {
        helpers::test_resolver_fail("using A = auto;",
                                    sema::diagnostic{"Type aliases cannot be 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 10UZ}});
    }

    SECTION("Function pointer type cannot have auto parameter") {
        helpers::test_resolver_fail(
            "var f: fn(auto): i32;",
            sema::diagnostic{"Function types cannot have 'auto' parameter types",
                             sema::error::ILLEGAL_AUTO_USAGE,
                             std::pair{0UZ, 10UZ}});
    }

    SECTION("Function pointer type cannot have auto return type") {
        helpers::test_resolver_fail(
            "var f: fn(i32): auto;",
            sema::diagnostic{"Function types cannot have 'auto' return type",
                             sema::error::ILLEGAL_AUTO_USAGE,
                             std::pair{0UZ, 16UZ}});
    }

    SECTION("Array type cannot have auto element type") {
        helpers::test_resolver_fail("var a: [5]auto;",
                                    sema::diagnostic{"Array elements cannot have type 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 10UZ}});

        helpers::test_resolver_fail("var a: []auto;",
                                    sema::diagnostic{"Array elements cannot have type 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 9UZ}});
    }
}

TEST_CASE("Function return type auto inference") {
    SECTION("Infers return type from return statement with value") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(): auto {
                return 42;
            };
            const result := f();
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_sym.get_kind_opt() == sema::symbol_kind::CALLABLE);
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::I32));

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("result", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::I32));
    }

    SECTION("Infers void when function body has no return statements") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(): auto {};
            const result := f();
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::VOID));

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("result", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::VOID));
    }

    SECTION("Infers void when function returns without expression") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(): auto {
                return;
            };
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::VOID));
    }

    SECTION("Infers return type from conditional branches") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(x: i32): auto {
                if (x > 0) {
                    return true;
                } else {
                    return false;
                }
            };
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::BOOL));
    }

    SECTION("Nested functions infer independent auto return types") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const outer := fn(): auto {
                const inner := fn(): auto {
                    return 100l;
                };
                return inner();
            };
            const result := outer();
        )")};

        const auto [out_sym, out_sym_data, out_node, out_type, out_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("outer",
                                                                                        idx)};
        CHECK(out_type_data.return_type == ctx->get_type(sema::type_kind::I64));

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("result", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::I64));
    }
}

} // namespace ghoti::tests
