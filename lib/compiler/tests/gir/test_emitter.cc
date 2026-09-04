#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

auto find_call_instruction(const gir::segment& seg) -> stdx::option<const gir::instruction&> {
    for (const auto* inst : seg.get_instructions()) {
        if (inst->kind == gir::instruction_kind::CALL) { return inst; }
    }
    return stdx::none;
}

} // namespace

TEST_CASE("Emitter top-level globals and type declarations") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const MAX_COUNT: i32 = 100;
        var current_count: i32 = 42;
        using Count = i32;
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // Verify types
    REQUIRE(gir_mod.get_types().size() == 1);
    CHECK(gir_mod.get_types()[0]->name == "Count");
    CHECK(gir_mod.get_types()[0]->type == ctx->analyzer.get_ctx().get_int(32, true));

    // Verify globals
    REQUIRE(gir_mod.get_globals().size() == 2);

    const auto& g_max{gir_mod.get_globals()[0]};
    CHECK(g_max->name == "MAX_COUNT");
    CHECK(g_max->is_constant);
    CHECK(UNWRAP(g_max->init_value).is<i64>());
    CHECK(UNWRAP(g_max->init_value).as<i64>() == 100);

    const auto& g_curr{gir_mod.get_globals()[1]};
    CHECK(g_curr->name == "current_count");
    CHECK_FALSE(g_curr->is_constant);
    CHECK(UNWRAP(g_curr->init_value).is<i64>());
    CHECK(UNWRAP(g_curr->init_value).as<i64>() == 42);
}

TEST_CASE("Emitter linear function with binary arithmetic") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const add := fn(a: i32, b: i32): i32 {
            return a + b;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "add");
    REQUIRE(fn.get_params().size() == 2);
    CHECK(fn.get_params()[0]->name == "a");
    CHECK(fn.get_params()[1]->name == "b");

    REQUIRE(fn.get_segments().size() == 1);
    const auto& entry{*fn.get_segments()[0]};
    REQUIRE(entry.get_instructions().size() == 2);

    // Instruction 0: %0: i32 = add param.0, param.1
    const auto& inst0{*entry.get_instructions()[0]};
    CHECK(inst0.kind == gir::instruction_kind::ADD);
    CHECK(UNWRAP(inst0.result).is_temp());
    REQUIRE(inst0.operands.size() == 2);
    CHECK(inst0.operands[0].is<gir::local_id>());
    CHECK(inst0.operands[0].as<gir::local_id>().is_param());
    CHECK(inst0.operands[1].is<gir::local_id>());
    CHECK(inst0.operands[1].as<gir::local_id>().is_param());

    // Instruction 1: ret %0
    const auto& inst1{*entry.get_instructions()[1]};
    CHECK(inst1.kind == gir::instruction_kind::RET);
    REQUIRE(inst1.operands.size() == 1);
    CHECK(inst1.operands[0].is<gir::local_id>());
    CHECK(inst1.operands[0].as<gir::local_id>() == *inst0.result);

    // Verify dump output
    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};
    CHECK(dump_text.contains("fn add(a: i32, b: i32) -> i32"));
    CHECK(dump_text.contains("%0 = add i32 param.0, param.1"));
    CHECK(dump_text.contains("ret i32 %0"));
}

TEST_CASE("Emitter local variable alloca, store, load, and compound assignment") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const compute := fn(x: i32): i32 {
            var acc: i32 = x;
            acc += 5;
            return acc;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "compute");

    REQUIRE(fn.get_segments().size() == 1);
    const auto& entry{*fn.get_segments()[0]};

    REQUIRE(entry.get_instructions().size() >= 6);

    CHECK(entry.get_instructions()[0]->kind == gir::instruction_kind::ALLOCA); // alloca slot.0: i32
    CHECK(entry.get_instructions()[1]->kind ==
          gir::instruction_kind::STORE); // store slot.0, param.0
    CHECK(entry.get_instructions()[2]->kind ==
          gir::instruction_kind::LOAD);                                     // %1: i32 = load slot.0
    CHECK(entry.get_instructions()[3]->kind == gir::instruction_kind::ADD); // %2: i32 = add %1, 5
    CHECK(entry.get_instructions()[4]->kind == gir::instruction_kind::STORE); // store slot.0, %2
    CHECK(entry.get_instructions()[5]->kind ==
          gir::instruction_kind::LOAD);                                     // %3: i32 = load slot.0
    CHECK(entry.get_instructions()[6]->kind == gir::instruction_kind::RET); // ret %3
}

TEST_CASE("Emitter unary operations and comparisons") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const check_negative := fn(val: i32): bool {
            return val < 0;
        };
        const negate_val := fn(val: i32): i32 {
            return -val;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);

    const auto& fn0{*gir_mod.get_functions()[0]};
    CHECK(fn0.get_name() == "check_negative");
    REQUIRE(fn0.get_segments().size() == 1);
    const auto& seg0{*fn0.get_segments()[0]};
    REQUIRE(seg0.get_instructions().size() == 2);
    CHECK(seg0.get_instructions()[0]->kind == gir::instruction_kind::LT);
    CHECK(seg0.get_instructions()[1]->kind == gir::instruction_kind::RET);

    const auto& fn1{*gir_mod.get_functions()[1]};
    CHECK(fn1.get_name() == "negate_val");
    REQUIRE(fn1.get_segments().size() == 1);
    const auto& seg1{*fn1.get_segments()[0]};
    REQUIRE(seg1.get_instructions().size() == 2);
    CHECK(seg1.get_instructions()[0]->kind == gir::instruction_kind::NEG);
    CHECK(seg1.get_instructions()[1]->kind == gir::instruction_kind::RET);
}

TEST_CASE("Emitter function calls") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const double_it := fn(x: i32): i32 {
            return x * 2;
        };
        const quad := fn(x: i32): i32 {
            return double_it(double_it(x));
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn_quad{*gir_mod.get_functions()[1]};
    CHECK(fn_quad.get_name() == "quad");
    REQUIRE(fn_quad.get_segments().size() == 1);
    const auto& seg{*fn_quad.get_segments()[0]};

    // Inner call, outer call, return
    REQUIRE(seg.get_instructions().size() == 3);
    CHECK(seg.get_instructions()[0]->kind == gir::instruction_kind::CALL);
    CHECK(seg.get_instructions()[1]->kind == gir::instruction_kind::CALL);
    CHECK(seg.get_instructions()[2]->kind == gir::instruction_kind::RET);
}

TEST_CASE("Emitter test blocks") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        test "simple addition test" {
            var res: i32 = 2 + 3;
        }
        test {
            var res: i32 = 2 + 3;
        }
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& test_fn{*gir_mod.get_functions()[0]};
    CHECK(test_fn.get_test_desc() == "simple addition test");
    CHECK(test_fn.get_is_test());

    const auto& anon_test_fn{*gir_mod.get_functions()[1]};
    CHECK(anon_test_fn.get_test_desc() == "anonymous_test0");
    CHECK(anon_test_fn.get_is_test());
}

TEST_CASE("Anonymous function expression and local lambda binding") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const run := fn(x: i32): i32 {
            const add_one := fn(v: i32): i32 {
                return v + 1;
            };
            return add_one(x);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // Expecting 2 functions: "run" and the nested "anonymous_fn0"
    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn0{*gir_mod.get_functions()[0]};
    CHECK(fn0.get_name() == "run");

    const auto& fn1{*gir_mod.get_functions()[1]};
    CHECK(fn1.get_name().starts_with("localfn."));
    REQUIRE(fn1.get_params().size() == 1);
    CHECK(fn1.get_params()[0]->name == "v");

    // The call inside run should target the local function
    const auto& run_seg{*fn0.get_segments()[0]};
    const auto& call_inst{UNWRAP(find_call_instruction(run_seg))};
    CHECK(call_inst.callee_name == fn1.get_name());
}

TEST_CASE("Immediate anonymous function invocation in expression position") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const calc := fn(a: i32): i32 {
            return (fn(x: i32): i32 { return x * 3; })(a);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);

    const auto& fn0{*gir_mod.get_functions()[0]};
    CHECK(fn0.get_name() == "calc");

    const auto& fn1{*gir_mod.get_functions()[1]};
    CHECK(fn1.get_name() == "anonymous_fn0");
    REQUIRE(fn1.get_params().size() == 1);
    CHECK(fn1.get_params()[0]->name == "x");

    const auto& calc_seg{*fn0.get_segments()[0]};
    REQUIRE(calc_seg.get_instructions().size() >= 2);
    const auto& call_inst{calc_seg.get_instructions()[0]};
    CHECK(call_inst->kind == gir::instruction_kind::CALL);
    CHECK(call_inst->callee_name == "anonymous_fn0");
}

TEST_CASE("Emitter struct method call with self parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct {
            x: i32,
            y: i32,
            pub const get_x := fn(&self): i32 {
                return self.x;
            };
        };
        const test_method := fn(p: Point): i32 {
            return p.get_x();
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() >= 2);
    const auto& get_x_fn{*gir_mod.get_functions()[0]};
    CHECK(get_x_fn.get_name() == "get_x");
    REQUIRE(get_x_fn.get_params().size() == 1);
    CHECK(get_x_fn.get_params()[0]->name == "self");

    const auto& test_fn{*gir_mod.get_functions()[1]};
    CHECK(test_fn.get_name() == "test_method");

    const auto& seg{*test_fn.get_segments()[0]};
    const auto& call_inst{UNWRAP(find_call_instruction(seg))};
    CHECK(call_inst.callee_name == "get_x");
    REQUIRE(call_inst.operands.size() == 1);
}

TEST_CASE("Emitter struct method call with explicit self parameter") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct {
            x: i32,
            y: i32,
            pub const get_x := fn(&self): i32 {
                return self.x;
            };
        };
        const test_explicit := fn(p: Point): i32 {
            return Point.get_x(&p);
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() >= 2);
    const auto& test_fn{*gir_mod.get_functions()[1]};
    CHECK(test_fn.get_name() == "test_explicit");

    const auto& seg{*test_fn.get_segments()[0]};
    const auto& call_inst{UNWRAP(find_call_instruction(seg))};
    CHECK(call_inst.callee_name == "get_x");
    REQUIRE(call_inst.operands.size() == 1);
}

TEST_CASE("Deferred array underlying a reference parameter resolves to a concrete array") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const bump_first := fn(a: &mut [3uz]mut i32): void {
            a[0] = a[0] + 5;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // `bump_first` plus the `weak` `panic_handler` pulled in for the `a[0]` bounds checks.
    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn{*gir_mod.get_functions()[0]};
    REQUIRE(fn.get_params().size() == 1);

    const auto& param_type{fn.get_params()[0]->type};
    REQUIRE(param_type.get_kind() == sema::type_kind::REFERENCE);
    const auto& ref_data{UNWRAP(param_type.get_data().as_opt<sema::types::reference>())};

    CHECK(ref_data.underlying.get_kind() == sema::type_kind::ARRAY);
    const auto& arr_data{UNWRAP(ref_data.underlying.get_data().as_opt<sema::types::array>())};
    CHECK(arr_data.len == 3);
    CHECK_FALSE(ref_data.underlying.is_constant());
}

} // namespace ghoti::tests
