#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

namespace {

// Checks for the builtin's presence in the prelude table and its return type when called
auto test_builtin_resolve(const syntax::builtin_t& builtin,
                          std::string_view         mock_params,
                          auto&&                   expected_type_fn,
                          std::string_view         prelude = "") -> void {
    auto [ctx, idx]{helpers::resolve_and_check(
        fmt::format("{}const foo := {}({});", prelude, builtin.name, mock_params))};

    // Quickly check to make sure the prelude table has the builtin
    const auto [builtin_sym, builtin_sym_data]{ctx->get_symbol<syms::builtin>(
        builtin.name, UNWRAP(ctx->analyzer.get_prelude_index_opt()))};
    CHECK(builtin_sym.get_kind_opt() == sema::symbol_kind::CALLABLE);
    const auto& builtin_type{builtin_sym_data.get_type()};
    CHECK(builtin_type.get_data().as_opt<sema::types::builtin_function>());

    // Now validate the actual declaration and call
    const sema::type& expected_type = expected_type_fn(*ctx);
    const auto [decl_sym, decl_sym_data, decl_node_data, decl_type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("foo", idx)};
    CHECK(expected_type == decl_type);

    const auto& call = UNWRAP(ctx->root_mod.ast.get_as_opt<ast::call_expr>(*decl_node_data.value));
    const auto& call_type{UNWRAP(ctx->root_mod.get_sema_type_opt(call.function))};
    CHECK(builtin_type == call_type);
}

} // namespace

namespace bis = syntax::builtins;

TEST_CASE("Builtin 'safe' casts") {
    const auto bi{GENERATE(bis::ALIGN_CAST, bis::PTR_CAST, bis::BIT_CAST, bis::AS)};
    test_builtin_resolve(bi, "i32, 23UZ", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_int_type(32, true);
    });
}

TEST_CASE("Builtin 'unsafe' casts") {
    test_builtin_resolve(
        bis::CONST_CAST, "^23i32", [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type<sema::types::mut::MUTABLE>(sema::type_kind::POINTER,
                                                           ctx.get_int_type(32, true));
        });

    test_builtin_resolve(
        bis::VOLATILE_CAST,
        "v",
        [](helpers::sema_test_context& ctx) -> sema::type& { return ctx.get_int_type(32, true); },
        "const v: volatile i32 = 23;");
}

TEST_CASE("Builtin bit/byte operations") {
    const auto bi{GENERATE(
        bis::ALIGN_OF, bis::SIZE_OF, bis::CLZ, bis::CTZ, bis::POP_COUNT, bis::INT_FROM_PTR)};
    test_builtin_resolve(bi, "123", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_type(sema::type_kind::USIZE);
    });
}

TEST_CASE("Builtin type introspection") {
    test_builtin_resolve(bis::TYPE_OF, "i32", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_type(sema::type_kind::TYPE, ctx.get_int_type(32, true));
    });

    test_builtin_resolve(bis::TAG_NAME, "123", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_type(sema::type_kind::SLICE, true, ctx.get_int_type(8, false));
    });
}

TEST_CASE("Builtin this introspection") {
    auto [ctx, idx]{helpers::resolve_and_check("struct { using A = @this(); };")};
    const auto [sym, data, type]{ctx->get_type_sym_info<syms::node_t>("A", idx + 1)};
    CHECK(type == ctx->get_type(sema::type_kind::STRUCT, idx + 1));
}

TEST_CASE("Deferred return type from typeOf") {
    auto [ctx,
          idx]{helpers::resolve_and_check("const a := fn(): type {}; using B = @typeOf(a());")};

    const auto [sym, sym_data, node_data, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::using_stmt>("B", idx)};

    const auto& call =
        UNWRAP(ctx->root_mod.ast.get_as_opt<ast::call_expr>(node_data.explicit_type));
    CHECK(type == ctx->get_type(sema::type_kind::TYPE, &call));
    CHECK(&call == &UNWRAP(type.get_data().as_opt<sema::types::deferred_call>()).call);
}

TEST_CASE("typeOf denotes the wrapped type in a value's or parameter's explicit type") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Alias := @typeOf(0);
        const via_alias: Alias = 7;
        const direct: @typeOf(false) = true;
        const echo := fn(x: auto, y: @typeOf(x)): auto { return y; };
        const call_echo := echo(1, 2);
    )")};

    const auto& i32_type{ctx->get_int_type(32, true)};
    const auto& cx_int{ctx->get_type(sema::type_kind::CONSTEXPR_INT)};
    const auto& bool_type{ctx->get_type(sema::type_kind::BOOL)};

    const auto decl_type = [&](std::string_view name) -> const sema::type& {
        const auto [sym, _, node, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        return type;
    };

    CHECK(decl_type("Alias") == ctx->get_type(sema::type_kind::TYPE, cx_int));
    CHECK(decl_type("via_alias") == cx_int);
    CHECK(decl_type("direct") == bool_type);
    CHECK(decl_type("call_echo") == i32_type);
}

TEST_CASE("A return type may depend on a parameter whose type depends on an earlier parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const chain := fn(a: auto, b: @typeOf(a)): @typeOf(b) { return b; };
        const call_chain := chain(1, 2);
    )")};

    const auto& i32_type{ctx->get_int_type(32, true)};
    const auto [sym, _, node, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("call_chain", idx)};
    CHECK(type == i32_type);
}

TEST_CASE("A later parameter's type may be a type-constructor call over an earlier parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Box := fn(T: type): type { return struct { val: T }; };
        const unbox := fn(a: auto, b: Box(@typeOf(a))): i32 { return b.val; };
        const boxed: Box(i32) = .{ .val = 2 };
        const call_unbox := unbox(1, boxed);
    )")};

    const auto& i32_type{ctx->get_int_type(32, true)};
    const auto [sym, _, node, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("call_unbox", idx)};
    CHECK(type == i32_type);
}

TEST_CASE("Builtin pointer conversions") {
    test_builtin_resolve(
        bis::PTR_FROM_ARRAY,
        "a",
        [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::POINTER, ctx.get_int_type(32, true));
        },
        "var a := [_]i32{0, 1, 2};");

    test_builtin_resolve(
        bis::PTR_FROM_INT, "^i32, 0xc0ffeeu64", [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::POINTER, ctx.get_int_type(32, true));
        });

    test_builtin_resolve(
        bis::SLICE_FROM_PTR, "^1i32, 20UZ", [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::SLICE, false, ctx.get_int_type(32, true));
        });
}

TEST_CASE("Builtins memory operation") {
    const auto bi{GENERATE(bis::MEMCPY, bis::MEMSET, bis::MEMMOVE)};
    test_builtin_resolve(
        bi,
        "a, b",
        [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::VOID_);
        },
        "var a: i32 = undefined; var b: i32 = undefined;");
}

TEST_CASE("Builtin arithmetic") {
    test_builtin_resolve(bis::ABS, "2.34f32", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_type(sema::type_kind::F32);
    });
}

TEST_CASE("SIMD Arithmetic") {
    test_builtin_resolve(
        bis::MUL_ADD, "f64, 1.0, 2.0, 3.0", [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::F64);
        });
}

TEST_CASE("Builtin control flow") {
    test_builtin_resolve(
        bis::PANIC, R"("Help!")", [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::NORETURN);
        });

    test_builtin_resolve(bis::TRAP, "", [](helpers::sema_test_context& ctx) -> sema::type& {
        return ctx.get_type(sema::type_kind::NORETURN);
    });
}

TEST_CASE("@panic requires a compile-time-constant message") {
    helpers::test_resolver_fail(
        R"(const f := fn(m: []u8): void { @panic(m); };)",
        sema::diagnostic{"@panic message must be a compile-time-constant string",
                         sema::error::CONSTEXPR_EVALUATION_FAILED,
                         std::pair{0UZ, 38UZ}});
}

TEST_CASE("Builtin function arity mismatch") {
    helpers::test_resolver_fail("const foo := @sizeOf();",
                                sema::diagnostic{"Builtin expects 1 arguments, found 0",
                                                 sema::error::ARITY_MISMATCH,
                                                 std::pair{0UZ, 13UZ}});
}

TEST_CASE("Const cast quick type checking") {
    helpers::test_resolver_fail(
        "const foo := @constCast(1);",
        sema::diagnostic{"Expected pointer, reference, slice, or array type; found 'constexpr_int'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 24UZ}});
}

TEST_CASE("Other builtin quick type mismatch") {
    helpers::test_resolver_fail(
        "const foo := @ptrFromArray(1i32);",
        sema::diagnostic{"Expected an array-yielding expression; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 27UZ}});

    helpers::test_resolver_fail("const foo := @ptrFromInt(i32, 0xdeadbeefu64);",
                                sema::diagnostic{"Expected a pointer type; found 'i32'",
                                                 sema::error::TYPE_MISMATCH,
                                                 std::pair{0UZ, 25UZ}});

    helpers::test_resolver_fail(
        "const foo := @sliceFromPtr(1i32, 20UZ);",
        sema::diagnostic{"Expected a pointer-yielding expression; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 27UZ}});
}

TEST_CASE("Illegal @this usage") {
    helpers::test_resolver_fail(
        "@this();",
        sema::diagnostic{"@this() may only be used inside of structs, unions, and enums",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 5UZ}});
}

TEST_CASE("Builtin C va builtins resolution") {
    test_builtin_resolve(
        bis::C_VA_START,
        "ap",
        [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::VOID_);
        },
        "const ap: ^mut opaque = undefined;");

    test_builtin_resolve(
        bis::C_VA_ARG,
        "ap, i32",
        [](helpers::sema_test_context& ctx) -> sema::type& { return ctx.get_int_type(32, true); },
        "const ap: ^mut opaque = undefined;");

    test_builtin_resolve(
        bis::C_VA_COPY,
        "dest, src",
        [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::VOID_);
        },
        "const dest: ^mut opaque = undefined; const src: ^mut opaque = undefined;");

    test_builtin_resolve(
        bis::C_VA_END,
        "ap",
        [](helpers::sema_test_context& ctx) -> sema::type& {
            return ctx.get_type(sema::type_kind::VOID_);
        },
        "const ap: ^mut opaque = undefined;");
}

TEST_CASE("@setEvalRecursionLimit top-level placement produces error") {
    helpers::test_resolver_fail(
        "const bad := @setEvalRecursionLimit(50);",
        sema::diagnostic{"@setEvalRecursionLimit can only be used within a function scope",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 13UZ}});
}

TEST_CASE("Free call and discard statements evaluate expressions without assignment") {
    SECTION("Free call @setMainSymbol") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            @setMainSymbol("custom_entry");
            pub const custom_entry := fn(): void {};
        )")};
        CHECK(ctx->analyzer.get_ctx().user_main_name == "custom_entry");
    }

    SECTION("Free call @setMainSymbol with leading underscore") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            @setMainSymbol("_my_custom_main");
            pub const _my_custom_main := fn(): void {};
        )")};
        CHECK(ctx->analyzer.get_ctx().user_main_name == "_my_custom_main");
    }

    SECTION("Discard statement with @setMainSymbol") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            _ = @setMainSymbol("discarded_main");
            pub const discarded_main := fn(): void {};
        )")};
        CHECK(ctx->analyzer.get_ctx().user_main_name == "discarded_main");
    }

    SECTION("Leading underscore identifier in declarations") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const _val: i32 = 42;
            pub const _get_val := fn(): i32 {
                return _val;
            };
        )")};
        CHECK(ctx->analyzer.get_ctx().diags.empty());
    }
}

TEST_CASE("@setMainSymbol invalid identifier error") {
    helpers::test_resolver_fail(
        "@setMainSymbol(\"invalid main name\");",
        sema::diagnostic{
            "@setMainSymbol argument must be a valid identifier; found 'invalid main name'",
            sema::error::TYPE_MISMATCH,
            std::pair{0UZ, 15UZ}});

    helpers::test_resolver_fail(
        "@setMainSymbol(\"123bad\");",
        sema::diagnostic{"@setMainSymbol argument must be a valid identifier; found '123bad'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 15UZ}});
}

} // namespace ghoti::tests
