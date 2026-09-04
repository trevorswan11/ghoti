#include <string>
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
#include "support/test.hh"

namespace ghoti::tests {

namespace syms = sema::symbols;

TEST_CASE("Primitive literal constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(
        "const a: i32 = 42; const b: bool = true; const c: f64 = 3.14;")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_a, _, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a", idx)};
    const auto val_a{UNWRAP(evaluator.try_eval(*decl_a.value))};
    CHECK(UNWRAP(val_a.as_opt<i64>()) == 42);

    const auto [sym_b, _b, decl_b, type_b]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("b", idx)};
    const auto val_b{UNWRAP(evaluator.try_eval(*decl_b.value))};
    CHECK(UNWRAP(val_b.as_opt<bool>()) == true);

    const auto [sym_c, _c, decl_c, type_c]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c", idx)};
    const auto val_c{UNWRAP(evaluator.try_eval(*decl_c.value))};
    CHECK(UNWRAP(val_c.as_opt<f64>()) == 3.14);
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
        const auto val{UNWRAP(evaluator.try_eval(*decl.value))};
        CHECK(UNWRAP(val.as_int_opt()) == expected);
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
        const auto val{UNWRAP(evaluator.try_eval(*decl.value))};
        CHECK(UNWRAP(val.as_opt<bool>()) == expected);
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
        const auto val{UNWRAP(evaluator.try_eval(*decl.value))};
        CHECK(UNWRAP(val.as_uint_opt()) == expected);
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

TEST_CASE("sizeOf/alignOf of pointer-width types follow the target's pointer width") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const PtrHolder := struct {
            p: ^i32,
            n: u8,
        };

        const sz_usize := @sizeOf(usize);
        const al_usize := @alignOf(usize);
        const sz_ptr := @sizeOf(^i32);
        const sz_slice := @sizeOf([]u8);
        const sz_struct := @sizeOf(PtrHolder);
    )")};

    const auto check_u64 = [&](std::string_view name, u64 expected) {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};
        const auto      val{UNWRAP(evaluator.try_eval(*decl.value))};
        CHECK(UNWRAP(val.as_uint_opt()) == expected);
    };

    // Default 64-bit host target: unchanged from before this fix.
    check_u64("sz_usize", 1 * sizeof(void*));
    check_u64("al_usize", 1 * sizeof(void*));
    check_u64("sz_ptr", 1 * sizeof(void*));
    check_u64("sz_slice", 2 * sizeof(void*));
    check_u64("sz_struct", 2 * sizeof(void*)); // p(8) + n(1), padded to 8-byte alignment

    ctx->analyzer.get_ctx().target_opts.triple_str = "i686-unknown-linux-gnu";
    check_u64("sz_usize", 4);
    check_u64("al_usize", 4);
    check_u64("sz_ptr", 4);
    check_u64("sz_slice", 8);
    check_u64("sz_struct", 8); // p(4) + n(1), padded to 4-byte alignment
}

TEST_CASE("Target builtins constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const os_name := @targetOs();
        const arch_name := @targetArch();
        const triple_name := @targetTriple();
        const abi_name := @targetAbi();
        const family_name := @targetFamily();
        const endian_name := @targetEndian();
        const ptr_bits := @targetPtrBits();
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto eval_of = [&](std::string_view name) -> gir::const_value {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        return UNWRAP(evaluator.try_eval(*decl.value));
    };

    const auto triple{eval_of("triple_name")};
    CHECK_FALSE(UNWRAP(triple.as_opt<std::string>()).empty());

    const auto enum_name_of = [&](std::string_view name) -> std::string {
        const auto value{eval_of(name)};
        return UNWRAP(value.as_opt<gir::const_enum>()).name;
    };

    CHECK_FALSE(enum_name_of("arch_name").empty());
    CHECK_FALSE(enum_name_of("abi_name").empty());
    CHECK_FALSE(enum_name_of("family_name").empty());

    const auto endian{enum_name_of("endian_name")};
    CHECK((endian == "little" || endian == "big"));

    // The normalized OS token must be version-suffix-free.
    const auto os{enum_name_of("os_name")};
    CHECK_FALSE(os.empty());
    CHECK(os.find_first_of("0123456789") == std::string::npos);

    const auto bits_val{eval_of("ptr_bits")};
    const auto bits{UNWRAP(bits_val.as_opt<u64>())};
    CHECK((bits == 64 || bits == 32 || bits == 16));
}

TEST_CASE("Target enum comparison folds against a bare member") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const here  := @targetOs();
        const eq    := @targetOs() == here;
        const ne    := @targetArch() != @targetArch();
        const probe := fn(o: builtin::Os): void {};
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto bool_of = [&](std::string_view name) -> bool {
        const auto [sym, _, decl, type]{
            ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>(name, idx)};
        const auto value{UNWRAP(evaluator.try_eval(*decl.value))};
        return UNWRAP(value.as_opt<bool>());
    };

    CHECK(bool_of("eq"));
    CHECK_FALSE(bool_of("ne"));
}

TEST_CASE("MulAdd and TagName constant eval") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const v_mul_add := @mulAdd(i32, 2, 3, 4);
        const Color := enum { RED, GREEN, BLUE };
        const tag := @tagName(Color.GREEN);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_ma, _, decl_ma, type_ma]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("v_mul_add", idx)};
    const auto val_ma{UNWRAP(evaluator.try_eval(*decl_ma.value))};
    CHECK(UNWRAP(val_ma.as_int_opt()) == 10);

    const auto [sym_tag, _t, decl_tag, type_tag]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("tag", idx)};
    const auto val_tag{UNWRAP(evaluator.try_eval(*decl_tag.value))};
    CHECK(UNWRAP(val_tag.as_opt<std::string>()) == "GREEN");
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
    const auto val_m{UNWRAP(evaluator.try_eval(*decl_m.value))};
    CHECK(UNWRAP(val_m.as_int_opt()) == 8);

    const auto [sym_k, _k, decl_k, type_k]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("K", idx)};
    const auto val_k{UNWRAP(evaluator.try_eval(*decl_k.value))};
    CHECK(UNWRAP(val_k.as_int_opt()) == 12);
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
    const auto arr_data{UNWRAP(resolved_type.get_data().as_opt<sema::types::array>())};
    CHECK(arr_data.len == 8);
    CHECK(sema::type_kind_display_name(arr_data.underlying) == "u8");
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
    const auto val_a{UNWRAP(evaluator.try_eval(*decl_a.value))};
    CHECK(UNWRAP(val_a.as_int_opt()) == 42);

    const auto [sym_m, _m, decl_m, type_m]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("res_max", idx)};
    const auto val_m{UNWRAP(evaluator.try_eval(*decl_m.value))};
    CHECK(UNWRAP(val_m.as_int_opt()) == 100);
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
    const auto val_s{UNWRAP(evaluator.try_eval(*decl_s.value))};
    CHECK(UNWRAP(val_s.as_int_opt()) == 55);

    const auto [sym_c, _c, decl_c, type_c]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("collatz_6", idx)};
    const auto val_c{UNWRAP(evaluator.try_eval(*decl_c.value))};
    CHECK(UNWRAP(val_c.as_int_opt()) == 8);
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
    const auto val_r{UNWRAP(evaluator.try_eval(*decl_r.value))};
    CHECK(UNWRAP(val_r.as_int_opt()) == 55);

    const auto [sym_a, _a, decl_a, type_a]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("a_sum", idx)};
    const auto val_a{UNWRAP(evaluator.try_eval(*decl_a.value))};
    CHECK(UNWRAP(val_a.as_int_opt()) == 60);
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
    const auto val_0{UNWRAP(evaluator.try_eval(*decl_0.value))};
    CHECK(UNWRAP(val_0.as_int_opt()) == 10);

    const auto [sym_2, _2, decl_2, type_2]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("elem2", idx)};
    const auto val_2{UNWRAP(evaluator.try_eval(*decl_2.value))};
    CHECK(UNWRAP(val_2.as_int_opt()) == 30);
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
    const auto val_x{UNWRAP(evaluator.try_eval(*decl_x.value))};
    CHECK(UNWRAP(val_x.as_int_opt()) == 15);

    const auto [sym_y, _y, decl_y, type_y]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("py", idx)};
    const auto val_y{UNWRAP(evaluator.try_eval(*decl_y.value))};
    CHECK(UNWRAP(val_y.as_int_opt()) == 25);
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
    const auto val_a{UNWRAP(evaluator.try_eval(*decl_a.value))};
    CHECK(UNWRAP(val_a.as_int_opt()) == 42);

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
    const auto val_0{UNWRAP(evaluator.try_eval(*decl_0.value))};
    CHECK(UNWRAP(val_0.as_int_opt()) == 100);

    const auto [sym_1, _1, decl_1, type_1]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c1", idx)};
    const auto val_1{UNWRAP(evaluator.try_eval(*decl_1.value))};
    CHECK(UNWRAP(val_1.as_int_opt()) == 200);

    const auto [sym_99, _99, decl_99, type_99]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("c99", idx)};
    const auto val_99{UNWRAP(evaluator.try_eval(*decl_99.value))};
    CHECK(UNWRAP(val_99.as_int_opt()) == 300);

    const auto [sym_r, _r, decl_r, type_r]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("code_red", idx)};
    const auto val_r{UNWRAP(evaluator.try_eval(*decl_r.value))};
    CHECK(UNWRAP(val_r.as_int_opt()) == 10);

    const auto [sym_b, _b, decl_b, type_b]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("code_blue", idx)};
    const auto val_b{UNWRAP(evaluator.try_eval(*decl_b.value))};
    CHECK(UNWRAP(val_b.as_int_opt()) == 30);
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

    const auto& resolved_a{UNWRAP(ctx->root_mod.get_sema_type_opt(node_a.explicit_type))};
    CHECK(sema::type_kind_display_name(resolved_a) == "i64");

    const auto& resolved_b{UNWRAP(ctx->root_mod.get_sema_type_opt(node_b.explicit_type))};
    CHECK(sema::type_kind_display_name(resolved_b) == "i32");
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
    const auto val_sz{UNWRAP(evaluator.try_eval(*decl_sz.value))};
    CHECK(UNWRAP(val_sz.as_int_opt()) == 4);

    const auto [sym_al, _al, decl_al, type_al]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("al", idx)};
    const auto val_al{UNWRAP(evaluator.try_eval(*decl_al.value))};
    CHECK(UNWRAP(val_al.as_int_opt()) == 4);
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
    CHECK_FALSE(val_rv);
}

TEST_CASE("Const eval string concatenation and comparison folding") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const greeting := "Hello, " + "world!";
        const is_same := "ghoti" == "ghoti";
        const is_diff := "ghoti" != "c";
        const is_less := "apple" < "banana";
        const char_idx := "ghoti"[1];
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_g, _g, decl_g, type_g]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("greeting", idx)};
    const auto val_g{UNWRAP(evaluator.try_eval(*decl_g.value))};
    CHECK(UNWRAP(val_g.as_opt<std::string>()) == "Hello, world!");

    const auto [sym_s, _s, decl_s, type_s]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("is_same", idx)};
    const auto val_s{UNWRAP(evaluator.try_eval(*decl_s.value))};
    CHECK(val_s.as<bool>() == true);

    const auto [sym_d, _d, decl_d, type_d]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("is_diff", idx)};
    const auto val_d{UNWRAP(evaluator.try_eval(*decl_d.value))};
    CHECK(val_d.as<bool>() == true);

    const auto [sym_l, _l, decl_l, type_l]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("is_less", idx)};
    const auto val_l{UNWRAP(evaluator.try_eval(*decl_l.value))};
    CHECK(val_l.as<bool>() == true);

    const auto [sym_c, _c, decl_c, type_c]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("char_idx", idx)};
    const auto val_c{UNWRAP(evaluator.try_eval(*decl_c.value))};
    CHECK(val_c.as_int_opt() == static_cast<i64>('h'));
}

TEST_CASE("Const eval @setEvalRecursionLimit in function scope") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        constexpr count_loop := fn(n: i32): i32 {
            @setEvalRecursionLimit(10);
            var i := 0;
            while (i < n) { i += 1; }
            return i;
        };
        const res := count_loop(5);
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_res, _res, decl_res, type_res]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("res", idx)};
    const auto val_res{UNWRAP(evaluator.try_eval(*decl_res.value))};
    CHECK(val_res.as_int_opt() == 5);
}

TEST_CASE("Const eval @setMainSymbol sets main symbol name") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const setup := @setMainSymbol("custom_entry");
    )")};
    gir::const_eval evaluator{ctx->analyzer.get_ctx(), ctx->root_mod};

    const auto [sym_s, _s, decl_s, type_s]{
        ctx->get_ast_type_sym_info<syms::node_t, ast::decl_stmt>("setup", idx)};
    CHECK(evaluator.try_eval(*decl_s.value));
    CHECK(ctx->analyzer.get_ctx().user_main_name == "custom_entry");
}

} // namespace ghoti::tests
