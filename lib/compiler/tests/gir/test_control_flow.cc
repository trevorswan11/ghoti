#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR if statement and if-else branching") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_if := fn(x: i32): i32 {
            var res: i32 = 0;
            if (x > 0) {
                res = 1;
            } else {
                res = -1;
            }
            return res;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "test_if");

    // Expect at least 4 segments: entry (0), consequence (1), alternate (2), merge (3)
    REQUIRE(fn.get_segments().size() >= 4);

    const auto& entry_seg{*fn.get_segments()[0]};
    REQUIRE(entry_seg.has_terminator());
    const auto& term0{*entry_seg.get_instructions().back()};
    CHECK(term0.kind == gir::instruction_kind::COND_GOTO);
    CHECK(term0.true_segment == gir::segment_id{1});
    CHECK(term0.false_segment == gir::segment_id{2});

    // Consequence segment jumps to merge (3)
    const auto& cons_seg{*fn.get_segments()[1]};
    REQUIRE(cons_seg.has_terminator());
    const auto& term1{*cons_seg.get_instructions().back()};
    CHECK(term1.kind == gir::instruction_kind::GOTO);
    CHECK(term1.target_segment == gir::segment_id{3});

    // Alternate segment jumps to merge (3)
    const auto& alt_seg{*fn.get_segments()[2]};
    REQUIRE(alt_seg.has_terminator());
    const auto& term2{*alt_seg.get_instructions().back()};
    CHECK(term2.kind == gir::instruction_kind::GOTO);
    CHECK(term2.target_segment == gir::segment_id{3});

    // Merge segment returns
    const auto& merge_seg{*fn.get_segments()[3]};
    REQUIRE(merge_seg.has_terminator());
    const auto& term3{*merge_seg.get_instructions().back()};
    CHECK(term3.kind == gir::instruction_kind::RET);
}

TEST_CASE("GIR if expression yielding value") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const choose := fn(c: bool, a: i32, b: i32): i32 {
            return if (c) a else b;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "choose");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("cond_goto param.0 seg 1, seg 2"));
    CHECK(dump_text.contains("goto seg 3"));
    CHECK(dump_text.contains("ret i32"));
}

TEST_CASE("GIR constexpr if branching") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_constexpr_if := fn(): i32 {
            if constexpr (true) {
                return 42;
            } else {
                return 0;
            }
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};

    // Comptime folding should emit only active branch, no cond_goto
    REQUIRE(fn.get_segments().size() == 1);
    const auto& seg{*fn.get_segments()[0]};
    REQUIRE(seg.has_terminator());
    CHECK(seg.get_instructions().back()->kind == gir::instruction_kind::RET);
}

TEST_CASE("GIR constexpr if error handling") {
    SECTION("Non-constant condition fails with CONSTEXPR_EVALUATION_FAILED") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const test_dyn_constexpr_if := fn(param: bool): i32 {
                if constexpr (param) {
                    return 1;
                } else {
                    return 2;
                }
            };
        )")};

        gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
        const auto   gir_mod{emitter.emit()};

        auto& diags{ctx->analyzer.get_ctx().diags};
        REQUIRE_FALSE(diags.empty());
        CHECK(diags[0].get_error() == sema::error::CONSTEXPR_EVALUATION_FAILED);
    }

    SECTION("Non-boolean condition fails with TYPE_MISMATCH") {
        auto [ctx, idx]{helpers::resolve_and_check(R"(
            const test_non_bool_constexpr_if := fn(): i32 {
                if constexpr (123) {
                    return 1;
                }
                return 0;
            };
        )")};

        gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
        const auto   gir_mod{emitter.emit()};

        auto& diags{ctx->analyzer.get_ctx().diags};
        REQUIRE_FALSE(diags.empty());
        CHECK(diags[0].get_error() == sema::error::TYPE_MISMATCH);
    }
}

TEST_CASE("GIR while loop with condition, body, and break") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const count_up := fn(max: i32): i32 {
            var i: i32 = 0;
            while (i < max) {
                i += 1;
                if (i == 5) {
                    break;
                }
            }
            return i;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "count_up");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("goto seg"));
    CHECK(dump_text.contains("ret i32"));
}

TEST_CASE("GIR while loop with continuation and else non_break") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const while_full := fn(limit: i32): i32 {
            var count: i32 = 0;
            var i: i32 = 0;
            while (i < limit) : (i += 1) {
                count += i;
            } else {
                count = -1;
            }
            return count;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "while_full");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("cond_goto"));
}

TEST_CASE("GIR do-while loop") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_do_while := fn(): i32 {
            var x: i32 = 0;
            do {
                x += 1;
            } while (x < 10);
            return x;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "test_do_while");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    // Body segment (1) is entered first, then cond_seg (2) jumps back to 1 or exits (3)
    CHECK(dump_text.contains("goto seg 1"));
    CHECK(dump_text.contains("cond_goto"));
}

TEST_CASE("GIR infinite loop with break and continue") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_infinite := fn(): i32 {
            var count: i32 = 0;
            loop {
                count += 1;
                if (count < 5) {
                    continue;
                }
                if (count == 10) {
                    break;
                }
            }
            return count;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "test_infinite");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("goto seg 1"));
    CHECK(dump_text.contains("ret i32"));
}

TEST_CASE("GIR for loop over range iterables") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const sum_range := fn(): i32 {
            var total: i32 = 0;
            for (0..10) |i| {
                total += i;
            }
            return total;
        };
        const sum_inclusive := fn(): i32 {
            var total: i32 = 0;
            for (1..=5) |j| {
                total += j;
            }
            return total;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);

    const auto& fn0{*gir_mod.get_functions()[0]};
    CHECK(fn0.get_name() == "sum_range");

    const auto& fn1{*gir_mod.get_functions()[1]};
    CHECK(fn1.get_name() == "sum_inclusive");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    // Both should contain comparisons (LT for .. and LE for ..=), step increments, and branch jumps
    CHECK(dump_text.contains("lt bool"));
    CHECK(dump_text.contains("le bool"));
    CHECK(dump_text.contains("add i32"));
}

TEST_CASE("Labeled GIR block yielding value via break") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const compute_labeled := fn(x: i32): i32 {
            const val := blk: {
                if (x > 0) {
                    break :blk 100;
                }
                break :blk 200;
            };
            return val;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "compute_labeled");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("store 100"));
    CHECK(dump_text.contains("store 200"));
    CHECK(dump_text.contains("ret i32"));
}

TEST_CASE("Labeled GIR nested loop break and continue") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const nested_loops := fn(): i32 {
            var count: i32 = 0;
            outer: for (0..5) |i| {
                for (0..5) |j| {
                    if (j == 2) {
                        continue :outer;
                    }
                    if (i == 3) {
                        break :outer;
                    }
                    count += 1;
                }
            }
            return count;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "nested_loops");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("fn nested_loops() -> i32"));
}

TEST_CASE("Short-circuit GIR boolean and / or expressions") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_and := fn(a: bool, b: bool): bool {
            return a and b;
        };
        const test_or := fn(a: bool, b: bool): bool {
            return a or b;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);

    const auto& fn_and{*gir_mod.get_functions()[0]};
    CHECK(fn_and.get_name() == "test_and");

    const auto& fn_or{*gir_mod.get_functions()[1]};
    CHECK(fn_or.get_name() == "test_or");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("fn test_and(a: bool, b: bool) -> bool"));
    CHECK(dump_text.contains("fn test_or(a: bool, b: bool) -> bool"));
    CHECK(dump_text.contains("cond_goto param.0"));
}

} // namespace ghoti::tests
