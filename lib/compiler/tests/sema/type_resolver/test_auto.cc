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
        var s := "hello";
    )")};

    const auto& i32_type{ctx->get_type(sema::type_kind::I32)};
    const auto& i64_type{ctx->get_type(sema::type_kind::I64)};
    const auto& bool_type{ctx->get_type(sema::type_kind::BOOL)};
    const auto& u8_type{ctx->get_type(sema::type_kind::U8)};
    const auto& str_type{ctx->get_type(sema::type_kind::ARRAY, true, 6UZ, u8_type)};

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

    SECTION("Walrus string declaration adopts string literal array type") {
        const auto [sym, data, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("s", idx)};
        CHECK(sym.get_kind_opt() == sema::symbol_kind::VALUE);
        CHECK(type == str_type);
        CHECK(ctx->root_mod.get_sema_type(node.name) == str_type);
    }
}

TEST_CASE("Declaration auto without initializer fails") {
    helpers::test_resolver_fail("var a: auto = undefined;",
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
        const auto struct_idx{UNWRAP(s_type.get_symbol_table_idx_opt(), 1UZ)};

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
            "var f: fn(x: auto): i32 = undefined;",
            sema::diagnostic{"Function types cannot have 'auto' parameter types",
                             sema::error::ILLEGAL_AUTO_USAGE,
                             std::pair{0UZ, 13UZ}});
    }

    SECTION("Function pointer type cannot have auto return type") {
        helpers::test_resolver_fail(
            "var f: fn(n: i32): auto = undefined;",
            sema::diagnostic{"Function types cannot have 'auto' return type",
                             sema::error::ILLEGAL_AUTO_USAGE,
                             std::pair{0UZ, 19UZ}});
    }

    SECTION("Array type cannot have auto element type") {
        helpers::test_resolver_fail("var a: [5]auto = undefined;",
                                    sema::diagnostic{"Array elements cannot have type 'auto'",
                                                     sema::error::ILLEGAL_AUTO_USAGE,
                                                     std::pair{0UZ, 10UZ}});

        helpers::test_resolver_fail("var a: []auto = undefined;",
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
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::VOID_));

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("result", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::VOID_));
    }

    SECTION("Infers void when function returns without expression") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(): auto {
                return;
            };
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::VOID_));
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

    SECTION("Infers return type from constexpr conditional branches") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const f := fn(): auto {
                if constexpr (true) {
                    return 42;
                } else {
                    return 100;
                }
            };
        )")};

        const auto [f_sym, f_sym_data, f_node, f_type, f_type_data]{
            ctx->get_full_sym_info<syms::node_t, ast::decl_stmt, sema::types::function>("f", idx)};
        CHECK(f_type_data.return_type == ctx->get_type(sema::type_kind::I32));
    }
}

TEST_CASE("Generic function instantiation and deduplication") {
    SECTION("Instantiates generic function with different parameter types") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const id := fn(x: auto): auto {
                return x;
            };
            const r1 := id(42);
            const r2 := id(true);
        )")};

        const auto [r1_sym, r1_sym_data, r1_node, r1_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r1", idx)};
        CHECK(r1_type == ctx->get_type(sema::type_kind::I32));

        const auto [r2_sym, r2_sym_data, r2_node, r2_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r2", idx)};
        CHECK(r2_type == ctx->get_type(sema::type_kind::BOOL));

        CHECK(ctx->root_mod.generic_instantiations.size() == 2);
        CHECK(ctx->root_mod.generic_instantiations[0].mangled_name == "id__i32");
        CHECK(ctx->root_mod.generic_instantiations[1].mangled_name == "id__bool");
    }

    SECTION("Deduplicates multiple calls with identical argument types") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const id := fn(x: auto): auto {
                return x;
            };
            const a := id(10);
            const b := id(20);
        )")};

        const auto [a_sym, a_sym_data, a_node, a_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
        CHECK(a_type == ctx->get_type(sema::type_kind::I32));

        const auto [b_sym, b_sym_data, b_node, b_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("b", idx)};
        CHECK(b_type == ctx->get_type(sema::type_kind::I32));

        CHECK(ctx->root_mod.generic_instantiations.size() == 1);
        CHECK(ctx->root_mod.generic_instantiations[0].mangled_name == "id__i32");
    }

    SECTION("Instantiates generic function with multiple auto parameters") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const add := fn(a: auto, b: auto): auto {
                return a + b;
            };
            const r1 := add(1, 2);
            const r2 := add(10l, 20l);
        )")};

        const auto [r1_sym, r1_sym_data, r1_node, r1_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r1", idx)};
        CHECK(r1_type == ctx->get_type(sema::type_kind::I32));

        const auto [r2_sym, r2_sym_data, r2_node, r2_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r2", idx)};
        CHECK(r2_type == ctx->get_type(sema::type_kind::I64));

        CHECK(ctx->root_mod.generic_instantiations.size() == 2);
        CHECK(ctx->root_mod.generic_instantiations[0].mangled_name == "add__i32_i32");
        CHECK(ctx->root_mod.generic_instantiations[1].mangled_name == "add__i64_i64");
    }

    SECTION("Instantiates generic function with mixed concrete and auto parameters") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const choose := fn(flag: bool, x: auto): auto {
                return x;
            };
            const r := choose(true, 99);
        )")};

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::I32));

        CHECK(ctx->root_mod.generic_instantiations.size() == 1);
        CHECK(ctx->root_mod.generic_instantiations[0].mangled_name == "choose__bool_i32");
    }
}

TEST_CASE("Generic function instantiation error handling") {
    SECTION("Type error inside generic function body poisons call site") {
        helpers::test_resolver_fail(
            R"(
            const bad := fn(x: auto): auto {
                return x.non_existent_field;
            };
            const res := bad(42);
        )",
            sema::diagnostic{
                "Can only access inner objects inside of structs, unions, and enums; found 'i32'",
                sema::error::TYPE_MISMATCH,
                std::pair{2UZ, 23UZ}});
    }
}

TEST_CASE("Cross-module generic function instantiation") {
    constexpr std::string_view math_gh{R"(
        pub const identity := fn(val: auto): auto {
            return val;
        };
        pub const double_val := fn(x: auto): auto {
            return x + x;
        };
    )"};

    auto [ctx, idx]{helpers::resolve_and_check(
        R"(
            import "math.gh" as math;
            const a := math::identity(42);
            const b := math::identity(true);
            const c := math::double_val(100l);
        )",
        helpers::make_vector<helpers::mock_file>(
            helpers::mock_file{.path = "math.gh", .source = math_gh}))};

    const auto [a_sym, a_sym_data, a_node, a_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    CHECK(a_type == ctx->get_type(sema::type_kind::I32));

    const auto [b_sym, b_sym_data, b_node, b_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("b", idx)};
    CHECK(b_type == ctx->get_type(sema::type_kind::BOOL));

    const auto [c_sym, c_sym_data, c_node, c_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c", idx)};
    CHECK(c_type == ctx->get_type(sema::type_kind::I64));
}

TEST_CASE("Generic functions with complex types and chaining") {
    SECTION("Generic function accepting struct parameter") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const Point := struct { x: i32, y: i32, };
            const get_x := fn(p: auto): auto {
                return p.x;
            };
            const pt := Point{ .x = 10, .y = 20 };
            const px := get_x(pt);
        )")};

        const auto [px_sym, px_sym_data, px_node, px_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("px", idx)};
        CHECK(px_type == ctx->get_type(sema::type_kind::I32));
    }

    SECTION("Generic function indexing slice/array") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const first := fn(arr: auto): auto {
                return arr[0];
            };
            const s := "hello";
            const c := first(s);
        )")};

        const auto [c_sym, c_sym_data, c_node, c_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c", idx)};
        CHECK(c_type == ctx->get_type(sema::type_kind::U8));
    }

    SECTION("Generic function with explicit non-auto return type") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const is_positive := fn(x: auto): bool {
                return x > 0;
            };
            const r := is_positive(10);
        )")};

        const auto [r_sym, r_sym_data, r_node, r_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r", idx)};
        CHECK(r_type == ctx->get_type(sema::type_kind::BOOL));
    }

    SECTION("Chained generic function calls") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const inner := fn(x: auto): auto {
                return x;
            };
            const outer := fn(y: auto): auto {
                return inner(y);
            };
            const res := outer(42);
        )")};

        const auto [res_sym, res_sym_data, res_node, res_type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("res", idx)};
        CHECK(res_type == ctx->get_type(sema::type_kind::I32));
    }
}

TEST_CASE("Dereferencing pointer and reference expressions") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const p: ^i32 = undefined;
        const r: &i32 = undefined;
        const pp: ^^i32 = undefined;

        const val_p := *p;
        const val_r := *r;
        const val_pp := **pp;
    )")};

    const auto [p_sym, _, p_decl, p_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("val_p", idx)};
    CHECK(p_type.get_kind() == sema::type_kind::I32);

    const auto [r_sym, _r, r_decl, r_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("val_r", idx)};
    CHECK(r_type.get_kind() == sema::type_kind::I32);

    const auto [pp_sym, _pp, pp_decl, pp_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("val_pp", idx)};
    CHECK(pp_type.get_kind() == sema::type_kind::I32);
}

} // namespace ghoti::tests
