#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/arena.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <string_view>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/gir.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR dumper formatting") {
    using namespace gir;

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_type{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             bool_type{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};
    auto&             fn_type{*pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};

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
            .kind     = instruction_kind::CONSTANT,
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
        CHECK(output.contains("%1 = constant i32 0"));
        CHECK(output.contains("%2 = lt bool %0, %1"));
        CHECK(output.contains("cond_goto %2 seg 1, seg 2"));
        CHECK(output.contains("seg 1:"));
        CHECK(output.contains("%3 = neg i32 %0"));
        CHECK(output.contains("ret i32 %3"));
        CHECK(output.contains("seg 2:"));
        CHECK(output.contains("ret i32 %0"));
    }
}

constexpr std::string_view golden_input{R"(
    using Real = f64;
    const MAX_SIZE := 100uz;

    const Point := struct {
        x: i32,
        y: i32,
    };

    const Color := enum {
        RED,
        GREEN,
        BLUE,
    };

    const clamp := fn(val: auto, min_val: auto, max_val: auto): auto {
        if (val < min_val) {
            return min_val;
        }
        if (val > max_val) {
            return max_val;
        }
        return val;
    };

    const compute_point := fn(p: Point): i32 {
        var acc: i32 = 0;
        defer acc = acc + 1;

        const clamped := clamp(p.x, 0, 50);
        var i: i32 = 0;
        while (i < 3) {
            acc += clamped;
            i += 1;
        }
        return acc + p.y;
    };

    const match_color := fn(c: Color): i32 {
        return match (c) {
            .RED => 1,
            .GREEN => 2,
            .BLUE => 3,
        };
    };

    const raw_write := fn(fd: i64, buf: ^u8, len: usize): i64 {
        var ret: i64 = 0l;
        asm {
            template: "syscall",
            outputs: ("={rax}" = ret),
            inputs: ("{rax}" = 1l, "{rdi}" = fd, "{rsi}" = buf, "{rdx}" = len),
            clobbers: ("rcx", "r11", "memory"),
            options: (volatile),
        };
        return ret;
    };

    test "golden_run" {
        const p := Point{ .x = 25, .y = 10 };
        const ans := compute_point(p);
        const code := match_color(.RED);
    }
)"};

constexpr std::string_view expected_gir{
#include "gir/dump.inc"
};

TEST_CASE("GIR comprehensive golden dump") {
    auto       ctx_idx{helpers::resolve_and_check(golden_input)};
    const auto dump{helpers::dump_gir(ctx_idx)};
    CHECK(dump == expected_gir);
}

} // namespace ghoti::tests
