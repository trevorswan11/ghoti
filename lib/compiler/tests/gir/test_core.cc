#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/arena.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/emitter.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/gir.hh"
#include "helpers/sema.hh"
#include "support/float128.hh"

namespace ghoti::tests {

TEST_CASE("GIR instruction kinds and terminator traits") {
    using namespace gir;

    CHECK_FALSE(is_terminator(instruction_kind::ALLOCA));
    CHECK_FALSE(is_terminator(instruction_kind::LOAD));
    CHECK_FALSE(is_terminator(instruction_kind::STORE));
    CHECK_FALSE(is_terminator(instruction_kind::ADD));
    CHECK_FALSE(is_terminator(instruction_kind::WIDEN_CAST));
    CHECK_FALSE(is_terminator(instruction_kind::CALL));

    CHECK(is_terminator(instruction_kind::RET));
    CHECK(is_terminator(instruction_kind::GOTO));
    CHECK(is_terminator(instruction_kind::COND_GOTO));
    CHECK(is_terminator(instruction_kind::UNREACHABLE));
}

TEST_CASE("GIR local_id representation") {
    using namespace gir;

    const auto temp_0{local_id::make_temp(0)};
    const auto temp_1{local_id::make_temp(1)};
    const auto param_0{local_id::make_param(0)};
    const auto alloca_0{local_id::make_alloca(0)};
    const auto global_0{local_id::make_global(0)};

    CHECK(temp_0.is_temp());
    CHECK(temp_0.get_index() == 0);
    CHECK_FALSE(temp_0.is_param());

    CHECK(param_0.is_param());
    CHECK(param_0.get_index() == 0);

    CHECK(alloca_0.is_alloca());
    CHECK(global_0.is_global());

    CHECK(temp_0 == local_id::make_temp(0));
    CHECK_FALSE(temp_0 == temp_1);
    CHECK_FALSE(temp_0 == param_0);
}

TEST_CASE("GIR value types and operations") {
    using namespace gir;

    ghoti::arena    arena;
    sema::type_pool types{arena};
    auto&           pool{types.intern()};

    const auto  loc{local_id::make_temp(42)};
    const value v_loc{loc};
    CHECK(v_loc.is_local());
    CHECK(v_loc.local() == loc);
    CHECK_FALSE(v_loc.is_literal<i64>(pool));

    const auto v_i64{value::constant(pool, i64{123})};
    CHECK(v_i64.is_constant());
    CHECK(UNWRAP(v_i64.literal_opt<i64>(pool)) == 123);
    CHECK_FALSE(v_i64.is_literal<u64>(pool));

    const auto v_u64{value::constant(pool, u64{456})};
    CHECK(UNWRAP(v_u64.literal_opt<u64>(pool)) == 456);

    const auto v_f64{value::constant(pool, f128::from_f64(3.14))};
    CHECK(UNWRAP(v_f64.literal_opt<f128>(pool)).to_f64() == 3.14);

    const auto v_bool{value::constant(pool, true)};
    CHECK(UNWRAP(v_bool.literal_opt<bool>(pool)) == true);

    const auto v_str{value::constant(pool, std::string{"hello"})};
    CHECK(UNWRAP(v_str.literal_opt<std::string>(pool)) == "hello");

    CHECK(value::constant(pool, void_val{}).is_literal<void_val>(pool));
    CHECK(value::constant(pool, undefined_val{}).is_literal<undefined_val>(pool));

    // Equal constants share a pool entry, so the operands compare equal
    CHECK(value::constant(pool, i64{123}) == v_i64);

    auto&      i32_type{*types[{sema::type_kind::INT, sema::types::mut::CONSTANT, u16{32}, true}]};
    const auto as_type{value::type_constant(pool, i32_type)};
    CHECK(as_type.is_type_constant(pool));
    CHECK(&UNWRAP(as_type.denoted_type_opt(pool)) == &i32_type);
    CHECK(value::type_constant(pool, stdx::none).is_type_constant(pool));
}

TEST_CASE("GIR segment terminator invariants") {
    using namespace gir;
    ghoti::arena arena;

    segment seg{arena, segment_id{0}};
    CHECK(seg.get_id() == segment_id{0});
    CHECK(seg.empty());
    CHECK(seg.size() == 0);
    CHECK_FALSE(seg.has_terminator());
    CHECK_FALSE(seg.get_terminator_opt());

    // Append non-terminator instruction
    seg.append({
        .kind     = instruction_kind::ALLOCA,
        .type     = nullptr,
        .result   = local_id::make_temp(0),
        .operands = {},
    });
    CHECK(seg.size() == 1);
    CHECK_FALSE(seg.has_terminator());

    // Append terminator instruction
    seg.append({
        .kind     = instruction_kind::RET,
        .type     = nullptr,
        .result   = stdx::none,
        .operands = helpers::make_vector<value>(local_id::make_temp(0)),
    });
    CHECK(seg.size() == 2);
    CHECK(seg.has_terminator());
    CHECK(UNWRAP(seg.get_terminator_opt()).kind == instruction_kind::RET);
}

TEST_CASE("GIR function management and local ID allocation") {
    using namespace gir;

    ghoti::arena    arena;
    sema::type_pool pool{arena};
    auto& i32_type{*pool[{sema::type_kind::INT, sema::types::mut::CONSTANT, u16{32}, true}]};
    auto& fn_type{*pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};

    function fn{arena, pool.intern(), "compute", fn_type};
    CHECK(fn.get_name() == "compute");
    CHECK_FALSE(fn.get_is_test());
    CHECK_FALSE(fn.get_is_comptime());

    fn.add_param("a", i32_type);
    fn.add_param("b", i32_type);
    REQUIRE(fn.get_params().size() == 2);
    CHECK(fn.get_params()[0]->name == "a");
    CHECK(fn.get_params()[0]->id == local_id::make_param(0));
    CHECK(fn.get_params()[1]->name == "b");
    CHECK(fn.get_params()[1]->id == local_id::make_param(1));

    const auto loc0{fn.next_local_id()};
    const auto loc1{fn.next_local_id()};
    CHECK(loc0.get_index() == 0);
    CHECK(loc1.get_index() == 1);
    CHECK(fn.local_count() == 2);

    fn.add_segment();
    fn.add_segment();
    REQUIRE(fn.get_segments().size() == 2);
    CHECK(fn.get_segments()[0]->get_id() == segment_id{0});
    CHECK(fn.get_segments()[1]->get_id() == segment_id{1});
    CHECK(fn.get_segment(segment_id{0})->get_id() == segment_id{0});
    CHECK(fn.get_segment(segment_id{1})->get_id() == segment_id{1});
}

TEST_CASE("GIR module container and arena allocation") {
    using namespace gir;

    auto [ctx, idx]{helpers::resolve_and_check("const a: i32 = 42;")};
    gir::module_t gir_mod{
        ctx->root_mod, ctx->analyzer.get_arena(), ctx->analyzer.get_pool().intern()};

    auto& i32_type{
        *ctx->analyzer
             .get_pool()[{sema::type_kind::INT, sema::types::mut::CONSTANT, u16{32}, true}]};
    auto& fn_type{
        *ctx->analyzer.get_pool()[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};

    // Add global
    gir_mod.add_global(
        "my_global", i32_type, true, value::constant(gir_mod.constants(), i64{42}, i32_type));
    CHECK(gir_mod.get_globals().size() == 1);
    CHECK(gir_mod.get_globals()[0]->name == "my_global");
    CHECK(gir_mod.get_globals()[0]->is_constant);

    // Add function
    gir_mod.add_function("main", fn_type);
    CHECK(gir_mod.get_functions().size() == 1);
    CHECK(gir_mod.get_tests().empty());

    // Add test function
    gir_mod.add_function("test_add", fn_type, true);
    CHECK(gir_mod.get_functions().size() == 2);
    CHECK(gir_mod.get_tests().size() == 1);
    CHECK(gir_mod.get_tests()[0] == 1);
}

TEST_CASE("Comprehensive program lowering") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point = struct {
            x: i32,
            y: i32,
        };

        const max = fn(a: auto, b: auto): auto {
            if (a > b) {
                return a;
            }
            return b;
        };

        const transform = fn(p: Point): i32 {
            let mut res: i32 = 0;
            defer res = res + 100;

            let mx = max(p.x, p.y);
            let mut i: i32 = 0;
            while (i < 3) {
                res += mx;
                i += 1;
            }

            return res;
        };

        test "run_transform" {
            let p = Point{ .x = 10, .y = 20 };
            let ans = transform(p);
        }
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    const auto dump_text{helpers::dump_gir(gir_mod)};
    CHECK(dump_text.contains("fn max__i32_i32"));
    CHECK(dump_text.contains("fn transform"));
    CHECK(dump_text.contains("test \"run_transform\""));
    CHECK(dump_text.contains("alloca"));
    CHECK(dump_text.contains("store"));
    CHECK(dump_text.contains("load"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("call @max__i32_i32"));
}

} // namespace ghoti::tests
