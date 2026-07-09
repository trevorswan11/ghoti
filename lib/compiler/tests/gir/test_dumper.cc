#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/arena.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("GIR dumper formatting") {
    using namespace gir;

    sema::type_pool pool{};
    auto&           i32_type{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&           bool_type{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};
    auto&           fn_type{*pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};
    stdx::arena<GIR_ARENA_BLOCK_SIZE> arena;

    SECTION("Dump linear function") {
        gir::function fn{arena, "add", fn_type};
        fn.add_param("a", i32_type);
        fn.add_param("b", i32_type);

        auto& seg{fn.add_segment()};
        seg.append({
            .kind     = instruction_kind::LOAD,
            .type     = &i32_type,
            .result   = local_id::make_temp(0),
            .operands = helpers::make_vector<value>(local_id::make_param(0)),
        });
        seg.append({
            .kind     = instruction_kind::LOAD,
            .type     = &i32_type,
            .result   = local_id::make_temp(1),
            .operands = helpers::make_vector<value>(local_id::make_param(1)),
        });
        seg.append({
            .kind     = instruction_kind::ADD,
            .type     = &i32_type,
            .result   = local_id::make_temp(2),
            .operands = helpers::make_vector<value>(local_id::make_temp(0), local_id::make_temp(1)),
        });
        seg.append({
            .kind     = instruction_kind::RET,
            .type     = &i32_type,
            .result   = stdx::none,
            .operands = helpers::make_vector<value>(local_id::make_temp(2)),
        });

        std::ostringstream ss;
        dumper             d{ss};
        d.dump(fn);

        const auto output{ss.view()};
        CHECK(output.contains("fn add(a: i32, b: i32)"));
        CHECK(output.contains("seg 0:"));
        CHECK(output.contains("%0 = load param.0"));
        CHECK(output.contains("%1 = load param.1"));
        CHECK(output.contains("%2 = add i32 %0, %1"));
        CHECK(output.contains("ret i32 %2"));
    }

    SECTION("Dump control flow function") {
        function fn{arena, "abs", fn_type};
        fn.add_param("x", i32_type);

        // seg 0
        auto& seg0{fn.add_segment()};
        seg0.append({
            .kind     = instruction_kind::LOAD,
            .type     = &i32_type,
            .result   = local_id::make_temp(0),
            .operands = helpers::make_vector<value>(local_id::make_param(0)),
        });
        seg0.append({
            .kind     = instruction_kind::CONST,
            .type     = &i32_type,
            .result   = local_id::make_temp(1),
            .operands = helpers::make_vector<value>(value{static_cast<i64>(0), &i32_type}),
        });
        seg0.append({
            .kind     = instruction_kind::LT,
            .type     = &bool_type,
            .result   = local_id::make_temp(2),
            .operands = helpers::make_vector<value>(local_id::make_temp(0), local_id::make_temp(1)),
        });
        seg0.append({
            .kind          = instruction_kind::COND_GOTO,
            .type          = nullptr,
            .result        = stdx::none,
            .operands      = helpers::make_vector<value>(local_id::make_temp(2)),
            .true_segment  = segment_id{1},
            .false_segment = segment_id{2},
        });

        // seg 1
        auto& seg1{fn.add_segment()};
        seg1.append({
            .kind     = instruction_kind::NEG,
            .type     = &i32_type,
            .result   = local_id::make_temp(3),
            .operands = helpers::make_vector<value>(local_id::make_temp(0)),
        });
        seg1.append({
            .kind     = instruction_kind::RET,
            .type     = &i32_type,
            .result   = stdx::none,
            .operands = helpers::make_vector<value>(local_id::make_temp(3)),
        });

        // seg 2
        auto& seg2{fn.add_segment()};
        seg2.append({
            .kind     = instruction_kind::RET,
            .type     = &i32_type,
            .result   = stdx::none,
            .operands = helpers::make_vector<value>(local_id::make_temp(0)),
        });

        std::ostringstream ss;
        dumper             d{ss};
        d.dump(fn);

        const auto output{ss.view()};
        CHECK(output.contains("fn abs(x: i32)"));
        CHECK(output.contains("seg 0:"));
        CHECK(output.contains("%0 = load param.0"));
        CHECK(output.contains("%1 = const i32 0"));
        CHECK(output.contains("%2 = lt bool %0, %1"));
        CHECK(output.contains("cond_goto %2 seg 1, seg 2"));
        CHECK(output.contains("seg 1:"));
        CHECK(output.contains("%3 = neg i32 %0"));
        CHECK(output.contains("ret i32 %3"));
        CHECK(output.contains("seg 2:"));
        CHECK(output.contains("ret i32 %0"));
    }
}

} // namespace ghoti::tests
