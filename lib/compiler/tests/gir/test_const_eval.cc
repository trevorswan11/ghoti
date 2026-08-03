#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Primitive literal constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(
        "const a: i32 = 42; const b: bool = true; const c: f64 = 3.14;")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_a, _, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    const auto val_a{evaluator.try_eval(*decl_a.value)};
    REQUIRE(val_a.has_value());
    CHECK(val_a->is<i64>());
    CHECK(val_a->as<i64>() == 42);

    const auto [sym_b, _b, decl_b, type_b]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("b", idx)};
    const auto val_b{evaluator.try_eval(*decl_b.value)};
    REQUIRE(val_b.has_value());
    CHECK(val_b->is<bool>());
    CHECK(val_b->as<bool>() == true);

    const auto [sym_c, _c, decl_c, type_c]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c", idx)};
    const auto val_c{evaluator.try_eval(*decl_c.value)};
    REQUIRE(val_c.has_value());
    CHECK(val_c->is<f64>());
    CHECK(val_c->as<f64>() == 3.14);
}

TEST_CASE("Integer arithmetic and bitwise folding constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const add := 2 + 3;
        const sub := 10 - 4;
        const mul := 10 * 3;
        const div := 10 / 2;
        const mod := 10 % 3;
        const neg := -(-3);
        const shl := 1 << 4;
        const shr := 16 >> 2;
        const band := 10 & 12;
        const bor := 10 | 5;
        const bxor := 10 ^ 12;
        const bnot := ~0;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto check_val = [&](std::string_view name, i64 expected) {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        const auto val{evaluator.try_eval(*decl.value)};
        REQUIRE(val.has_value());
        CHECK(val->as_int_opt() == expected);
    };

    check_val("add", 2 + 3);
    check_val("sub", 10 - 4);
    check_val("mul", 10 * 3);
    check_val("div", 10 / 2);
    check_val("mod", 10 % 3);
    check_val("neg", -(-3));
    check_val("shl", 1 << 4);
    check_val("shr", 16 >> 2);
    check_val("band", 10 & 12);
    check_val("bor", 10 | 5);
    check_val("bxor", 10 ^ (12));
    check_val("bnot", ~0);
}

TEST_CASE("Boolean logic and comparisons constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const t_and := true and false;
        const t_or := true or false;
        const t_not := !true;
        const t_gt := 5 > 3;
        const t_lt := 5 < 3;
        const t_eq := 5 == 5;
        const t_neq := 5 != 4;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto check_bool = [&](std::string_view name, bool expected) {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        const auto val{evaluator.try_eval(*decl.value)};
        REQUIRE(val.has_value());
        CHECK(val->as<bool>() == expected);
    };

    check_bool("t_and", true && false);
    check_bool("t_or", true || false);
    check_bool("t_not", !true);
    check_bool("t_gt", 5 > 3);
    check_bool("t_lt", 5 < 3);
    check_bool("t_eq", 5 == 5);
    check_bool("t_neq", 5 != 4);
}

TEST_CASE("Compile-time builtins constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const sz_u8 := @sizeOf(u8);
        const sz_u32 := @sizeOf(u32);
        const sz_u64 := @sizeOf(u64);
        const sz_arr := @sizeOf([4]u8);
        const al_u32 := @alignOf(u32);
        const al_u64 := @alignOf(u64);
        const v_abs := @abs(-42);
        const v_clz := @clz(1);
        const v_pop := @popCount(7);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto check_u64 = [&](std::string_view name, u64 expected) {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        const auto val{evaluator.try_eval(*decl.value)};
        REQUIRE(val.has_value());
        CHECK(val->as_uint_opt() == expected);
    };

    check_u64("sz_u8", 1);
    check_u64("sz_u32", 4);
    check_u64("sz_u64", 8);
    check_u64("sz_arr", 4);
    check_u64("al_u32", 4);
    check_u64("al_u64", 8);
    check_u64("v_abs", 42);
    check_u64("v_clz", 63);
    check_u64("v_pop", 3);
}

TEST_CASE("Target builtins constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const os_name := @targetOs();
        const arch_name := @targetArch();
        const triple_name := @targetTriple();
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto check_str = [&](std::string_view name) {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        const auto val{evaluator.try_eval(*decl.value)};
        REQUIRE(val.has_value());
        const auto str_opt{val->as_opt<std::string>()};
        REQUIRE(str_opt.has_value());
        CHECK(!str_opt->empty());
    };

    check_str("os_name");
    check_str("arch_name");
    check_str("triple_name");
}

TEST_CASE("Const symbol reference propagation constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const N := 4;
        const M := N * 2;
        const K := M + N;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_m, _m, decl_m, type_m]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("M", idx)};
    const auto val_m{evaluator.try_eval(*decl_m.value)};
    REQUIRE(val_m.has_value());
    CHECK(val_m->as_int_opt() == 8);

    const auto [sym_k, _k, decl_k, type_k]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("K", idx)};
    const auto val_k{evaluator.try_eval(*decl_k.value)};
    REQUIRE(val_k.has_value());
    CHECK(val_k->as_int_opt() == 12);
}

TEST_CASE("Array dimension resolution constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const N := 4;
        const a: [N * 2]u8 = [8]u8{1, 2, 3, 4, 5, 6, 7, 8};
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    evaluator.resolve_all_deferred_arrays();

    const auto [sym, _, decl, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    const auto  explicit_type_id{*decl.explicit_type};
    const auto& resolved_type{ctx->root_mod.get_sema_type(explicit_type_id)};

    CHECK(resolved_type.get_kind() == sema::type_kind::ARRAY);
    const auto arr_data{resolved_type.get_data().as_opt<sema::types::array>()};
    REQUIRE(arr_data.has_value());
    CHECK(arr_data->len == 8);
    CHECK(arr_data->underlying.get_kind() == sema::type_kind::U8);
}

TEST_CASE("Const eval function evaluation") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        constexpr add := fn(a: i32, b: i32): i32 {
            const sum := a + b;
            return sum;
        };
        constexpr max_val := fn(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            }
        };
        const res_add := add(40, 2);
        const res_max := max_val(100, 42);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_a, _a, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("res_add", idx)};
    const auto val_a{evaluator.try_eval(*decl_a.value)};
    REQUIRE(val_a.has_value());
    CHECK(val_a->as_int_opt() == 42);

    const auto [sym_m, _m, decl_m, type_m]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("res_max", idx)};
    const auto val_m{evaluator.try_eval(*decl_m.value)};
    REQUIRE(val_m.has_value());
    CHECK(val_m->as_int_opt() == 100);
}

TEST_CASE("Const eval variable mutation and loops") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        constexpr loop_sum := fn(n: i32): i32 {
            var i := 1;
            var sum := 0;
            while (i <= n) {
                sum += i;
                i += 1;
            }
            return sum;
        };
        constexpr collatz := fn(start: i32): i32 {
            var n := start;
            var steps := 0;
            while (n > 1) {
                if (n % 2 == 0) {
                    n /= 2;
                } else {
                    n = 3 * n + 1;
                }
                steps += 1;
            }
            return steps;
        };
        const sum_10 := loop_sum(10);
        const collatz_6 := collatz(6);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_s, _s, decl_s, type_s]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("sum_10", idx)};
    const auto val_s{evaluator.try_eval(*decl_s.value)};
    REQUIRE(val_s.has_value());
    CHECK(val_s->as_int_opt() == 55);

    const auto [sym_c, _c, decl_c, type_c]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("collatz_6", idx)};
    const auto val_c{evaluator.try_eval(*decl_c.value)};
    REQUIRE(val_c.has_value());
    CHECK(val_c->as_int_opt() == 8);
}

TEST_CASE("Const eval for loops over ranges and arrays") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        constexpr range_sum := fn(n: i32): i32 {
            var sum := 0;
            for (1..(n + 1)) |i| {
                sum += i;
            }
            return sum;
        };
        constexpr arr_sum := fn(): i32 {
            var sum := 0;
            for (0..3, [_]i32{10, 20, 30}) |i, val| {
                sum += val;
            }
            return sum;
        };
        const r_sum_10 := range_sum(10);
        const a_sum := arr_sum();
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_r, _r, decl_r, type_r]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("r_sum_10", idx)};
    const auto val_r{evaluator.try_eval(*decl_r.value)};
    REQUIRE(val_r.has_value());
    CHECK(val_r->as_int_opt() == 55);

    const auto [sym_a, _a, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a_sum", idx)};
    const auto val_a{evaluator.try_eval(*decl_a.value)};
    REQUIRE(val_a.has_value());
    CHECK(val_a->as_int_opt() == 60);
}

TEST_CASE("Array constant eval indexing") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const arr := [_]i32{10, 20, 30, 40};
        const elem0 := arr[0];
        const elem2 := arr[2];
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_0, _0, decl_0, type_0]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("elem0", idx)};
    const auto val_0{evaluator.try_eval(*decl_0.value)};
    REQUIRE(val_0.has_value());
    CHECK(val_0->as_int_opt() == 10);

    const auto [sym_2, _2, decl_2, type_2]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("elem2", idx)};
    const auto val_2{evaluator.try_eval(*decl_2.value)};
    REQUIRE(val_2.has_value());
    CHECK(val_2->as_int_opt() == 30);
}

TEST_CASE("Struct member constant eval access") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct { x: i32, y: i32 };
        const pt := Point{ .x = 15, .y = 25 };
        const px := pt.x;
        const py := pt.y;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_x, _x, decl_x, type_x]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("px", idx)};
    const auto val_x{evaluator.try_eval(*decl_x.value)};
    REQUIRE(val_x.has_value());
    CHECK(val_x->as_int_opt() == 15);

    const auto [sym_y, _y, decl_y, type_y]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("py", idx)};
    const auto val_y{evaluator.try_eval(*decl_y.value)};
    REQUIRE(val_y.has_value());
    CHECK(val_y->as_int_opt() == 25);
}

TEST_CASE("Union constant eval active and inactive member access") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Payload := union { int_val: i32, float_val: f64 };
        const u := Payload{ .int_val = 42 };
        const active_val := u.int_val;
        const inactive_val := u.float_val;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_a, _a, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("active_val", idx)};
    const auto val_a{evaluator.try_eval(*decl_a.value)};
    REQUIRE(val_a.has_value());
    CHECK(val_a->as_int_opt() == 42);

    const auto [sym_i, _i, decl_i, type_i]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("inactive_val", idx)};
    const auto val_i{evaluator.eval(*decl_i.value)};
    CHECK(val_i.is_poison());
    CHECK_FALSE(ctx->analyzer.get_ctx().diags.empty());
}

TEST_CASE("Match constant eval expression evaluation") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        constexpr classify := fn(x: i32): i32 {
            return match (x) {
                0 => 100,
                1 => 200,
                _ => 300,
            };
        };
        const Color := enum { RED, GREEN, BLUE };
        constexpr color_code := fn(c: Color): i32 {
            return match (c) {
                .RED => 10,
                .GREEN => 20,
                .BLUE => 30,
            };
        };
        const c0 := classify(0);
        const c1 := classify(1);
        const c99 := classify(99);
        const code_red := color_code(Color.RED);
        const code_blue := color_code(.BLUE);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_0, _0, decl_0, type_0]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c0", idx)};
    const auto val_0{evaluator.try_eval(*decl_0.value)};
    REQUIRE(val_0.has_value());
    CHECK(val_0->as_int_opt() == 100);

    const auto [sym_1, _1, decl_1, type_1]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c1", idx)};
    const auto val_1{evaluator.try_eval(*decl_1.value)};
    REQUIRE(val_1.has_value());
    CHECK(val_1->as_int_opt() == 200);

    const auto [sym_99, _99, decl_99, type_99]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c99", idx)};
    const auto val_99{evaluator.try_eval(*decl_99.value)};
    REQUIRE(val_99.has_value());
    CHECK(val_99->as_int_opt() == 300);

    const auto [sym_r, _r, decl_r, type_r]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("code_red", idx)};
    const auto val_r{evaluator.try_eval(*decl_r.value)};
    REQUIRE(val_r.has_value());
    CHECK(val_r->as_int_opt() == 10);

    const auto [sym_b, _b, decl_b, type_b]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("code_blue", idx)};
    const auto val_b{evaluator.try_eval(*decl_b.value)};
    REQUIRE(val_b.has_value());
    CHECK(val_b->as_int_opt() == 30);
}

TEST_CASE("Resolve deferred call returning type") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const choose_type := fn(is_64: bool): type {
            if (is_64) {
                return i64;
            } else {
                return i32;
            }
        };
        using TypeA = choose_type(true);
        using TypeB = choose_type(false);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_a, _a, node_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::using_stmt>("TypeA", idx)};
    CHECK(type_a.get_data().is<sema::types::deferred_call>());

    const auto [sym_b, _b, node_b, type_b]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::using_stmt>("TypeB", idx)};
    CHECK(type_b.get_data().is<sema::types::deferred_call>());

    evaluator.resolve_all_deferred_types();

    const auto resolved_a{ctx->root_mod.get_sema_type_opt(node_a.explicit_type)};
    REQUIRE(resolved_a.has_value());
    CHECK(resolved_a->get_kind() == sema::type_kind::I64);

    const auto resolved_b{ctx->root_mod.get_sema_type_opt(node_b.explicit_type)};
    REQUIRE(resolved_b.has_value());
    CHECK(resolved_b->get_kind() == sema::type_kind::I32);
}

TEST_CASE("Division by zero failure handling in constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check("const bad := 10 / 0;")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym, _, decl, type]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("bad", idx)};
    const auto val{evaluator.eval(*decl.value)};
    CHECK(val.is_poison());
    CHECK_FALSE(ctx->analyzer.get_ctx().diags.empty());
}

TEST_CASE("Builtin const eval @this") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Node := struct {
            val: i32,

            using Self = @this();
            pub const bar := fn(s: &Self): i32 { return 0; };
        };
        const sz := @sizeOf(Node);
        const al := @alignOf(Node);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_sz, _sz, decl_sz, type_sz]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("sz", idx)};
    const auto val_sz{evaluator.try_eval(*decl_sz.value)};
    REQUIRE(val_sz.has_value());
    CHECK(val_sz->as_int_opt() == 4);

    const auto [sym_al, _al, decl_al, type_al]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("al", idx)};
    const auto val_al{evaluator.try_eval(*decl_al.value)};
    REQUIRE(val_al.has_value());
    CHECK(val_al->as_int_opt() == 4);
}

TEST_CASE("Volatile variables refuse constant folding in const eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const v: volatile i32 = 42;
        const read_v := v;
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_rv, _rv, decl_rv, type_rv]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("read_v", idx)};
    const auto val_rv{evaluator.try_eval(*decl_rv.value)};
    CHECK_FALSE(val_rv.has_value());
}

} // namespace ghoti::tests
