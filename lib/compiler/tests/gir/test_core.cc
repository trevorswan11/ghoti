#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/arena.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

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

    CHECK(instruction_kind_name(instruction_kind::ALLOCA) == "alloca");
    CHECK(instruction_kind_name(instruction_kind::LOAD) == "load");
    CHECK(instruction_kind_name(instruction_kind::STORE) == "store");
    CHECK(instruction_kind_name(instruction_kind::ADD) == "add");
    CHECK(instruction_kind_name(instruction_kind::RET) == "ret");
    CHECK(instruction_kind_name(instruction_kind::GOTO) == "goto");
    CHECK(instruction_kind_name(instruction_kind::COND_GOTO) == "cond_goto");
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

    const auto  loc{local_id::make_temp(42)};
    const value v_loc{loc};
    CHECK(v_loc.is<local_id>());
    CHECK(v_loc.as<local_id>() == loc);
    CHECK_FALSE(v_loc.is<i64>());

    const value v_i64{i64{123}};
    CHECK(v_i64.is<i64>());
    CHECK(UNWRAP(v_i64.as_opt<i64>()) == 123);

    const value v_u64{u64{456}};
    CHECK(v_u64.is<u64>());
    CHECK(UNWRAP(v_u64.as_opt<u64>()) == 456);

    const value v_f64{3.14};
    CHECK(v_f64.is<f64>());
    CHECK(UNWRAP(v_f64.as_opt<f64>()) == 3.14);

    const value v_bool{true};
    CHECK(v_bool.is<bool>());
    CHECK(UNWRAP(v_bool.as_opt<bool>()) == true);

    const value v_str{std::string{"hello"}};
    CHECK(v_str.is<std::string>());
    CHECK(UNWRAP(v_str.as_opt<std::string>()) == "hello");

    const value v_void{void_val{}};
    CHECK(v_void.is<void_val>());

    const value v_undef{undefined_val{}};
    CHECK(v_undef.is<undefined_val>());
}

TEST_CASE("GIR segment terminator invariants") {
    using namespace gir;
    sema::arena_alloc arena;

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

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_type{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             fn_type{*pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};

    function fn{arena, "compute", fn_type};
    CHECK(fn.get_name() == "compute");
    CHECK_FALSE(fn.get_is_test());
    CHECK_FALSE(fn.get_is_constexpr());

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
    gir::module gir_mod{ctx->root_mod, ctx->analyzer.get_arena()};

    auto& i32_type{*ctx->analyzer.get_pool()[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto& fn_type{
        *ctx->analyzer.get_pool()[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};

    // Add global
    gir_mod.add_global("my_global", i32_type, true, value{static_cast<i64>(42), &i32_type});
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
        const Point := struct {
            x: i32,
            y: i32,
        };

        const max := fn(a: auto, b: auto): auto {
            if (a > b) {
                return a;
            }
            return b;
        };

        const transform := fn(p: Point): i32 {
            var res: i32 = 0;
            defer res = res + 100;

            const mx := max(p.x, p.y);
            var i: i32 = 0;
            while (i < 3) {
                res += mx;
                i += 1;
            }

            return res;
        };

        test "run_transform" {
            const p := Point{ .x = 10, .y = 20 };
            const ans := transform(p);
        }
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("fn max__i32_i32"));
    CHECK(dump_text.contains("fn transform"));
    CHECK(dump_text.contains("test \"run_transform\""));
    CHECK(dump_text.contains("alloca"));
    CHECK(dump_text.contains("store"));
    CHECK(dump_text.contains("load"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("call @max__i32_i32"));
    CHECK(dump_text.contains("call @transform"));
}

} // namespace ghoti::tests
