#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Labeled for loop resolution") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        var arr: []bool;
        const a := outer: for (0..5, blk: { break :blk 2..4; }, arr) |i, j, _| {
            const foo := 39 + j;
            if (foo == 42) { break :outer 27; }
        } else {
            const foo := 42;
            if (foo + 1 == 42) { break :outer 26; }
        };
    )")};
    // +6 for the injected target-fact enum module (root + one scope per enum).
    CHECK(ctx->analyzer.get_registry().size() == 15);

    const auto& i32_type{ctx->get_type(sema::type_kind::I32)};
    const auto [a_sym, a_sym_data, a_type]{ctx->get_type_sym_info<syms::node_t>("a", idx)};
    CHECK(a_type == i32_type);

    {
        const auto [sym, sym_data, node_data, type]{
            ctx->get_ast_type_sym_info<syms::label, ast::label_expr>(
                "outer", idx + 1, stdx::none, &syms::label::get_definition)};
        CHECK(ctx->root_mod.get_sema_type(node_data.name) == i32_type);
    }

    const auto check_capture = [&](std::string_view name) -> void {
        const auto [sym, sym_data, type]{ctx->get_type_sym_info<syms::for_loop_capture>(
            name, 4, stdx::none, &syms::for_loop_capture::payload)};
        CHECK(type == i32_type);
    };
    check_capture("i");
    check_capture("j");
}

TEST_CASE("Complex label resolution") {
    helpers::resolve_and_check(
        "const a := do { const foo := 42; } while (blk: { break :blk 42; });");
    helpers::resolve_and_check(R"(
        var i: i32;
        const a := outer: while (blk: { break :blk 42; }) : (i += blk: {
            break :blk 42;
        }) {
            break :outer if (i == 42) 3 else blk: {
                const foo := 42;
                break :blk 42;
            };
        } else { const foo := 42; };
    )");
}

TEST_CASE("Label multi-type resolution") {
    SECTION("Multi control flow") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
blk: {
    break :blk 42;
    break :blk true;
    break :blk "val";
    break :blk;
};)")};

        const auto [_, data]{ctx->get_symbol<syms::label>("blk", idx + 1)};
        const auto yield_types{data.get_yield_types()};
        CHECK(yield_types.size() == 4);
        CHECK(*yield_types[0] == ctx->get_type(sema::type_kind::I32));
        CHECK(*yield_types[1] == ctx->get_type(sema::type_kind::BOOL));
        CHECK(*yield_types[2] ==
              ctx->get_type(sema::type_kind::ARRAY, true, 4, ctx->get_type(sema::type_kind::U8)));
        CHECK(*yield_types[3] == ctx->get_type(sema::type_kind::VOID_));
    }

    SECTION("Only continue") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
loop {
    blk: {
        continue :blk;
    };
})")};

        const auto [_, data]{ctx->get_symbol<syms::label>("blk", idx + 2)};
        const auto yield_types{data.get_yield_types()};
        CHECK(yield_types.size() == 1);
        CHECK(*yield_types[0] == ctx->get_type(sema::type_kind::VOID_));
    }
}

TEST_CASE("Unknown labels resolved") {
    const auto test_unknown_label = [](std::string_view name, usize col) -> void {
        helpers::test_resolver_fail(
            fmt::format("for (0..2) |_| {{ {} :blk; }}", name),
            sema::diagnostic{
                fmt::format("Labeled {} statements must be used with a known label", name),
                sema::error::ILLEGAL_CONTROL_FLOW,
                std::pair{0UZ, col}});
    };

    test_unknown_label("break", 24);
    test_unknown_label("continue", 27);
}

} // namespace ghoti::tests
