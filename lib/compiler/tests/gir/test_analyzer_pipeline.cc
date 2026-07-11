#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "catch2/catch_message.hpp"
#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR array indexing bounds checking") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const get_elem := fn(arr: [5]i32, i: usize): i32 {
            return arr[i];
        };

        const set_elem := fn(arr: [5]i32, i: usize, val: i32): void {
            arr[i] = val;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 2);
    for (const auto& fn : gir_mod.get_functions()) {
        std::ostringstream ss;
        gir::dumper        dumper{ss};
        dumper.dump(*fn);
        const auto dump_text{ss.view()};

        // Bounds checking must emit LT comparison with size (5), cond_goto, and unreachable
        CHECK(dump_text.contains("lt bool"));
        CHECK(dump_text.contains("cond_goto"));
        CHECK(dump_text.contains("unreachable"));
        CHECK(dump_text.contains("get_element_ptr"));
    }
}

TEST_CASE("GIR array literal stack allocation and initialization") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_array_lit := fn(): i32 {
            var a: [3]i32 = [3]i32{ 10, 20, 30 };
            return a[1];
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};
    UNSCOPED_INFO("DUMP:\n" << dump_text);

    CHECK(dump_text.contains("alloca"));
    CHECK(dump_text.contains("get_element_ptr"));
}

TEST_CASE("GIR builtins cast operations") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_casts := fn(x: i32, ptr: ^i32): usize {
            const w := @as(i64, x);
            const u := @intFromPtr(ptr);
            return u;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("widen_cast"));
    CHECK(dump_text.contains("int_from_ptr"));
}

TEST_CASE("GIR emit_gir integration in pipeline") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const compute := fn(a: i32, b: i32): i32 {
            return a * b + 1;
        };
    )")};

    const auto gir_mod{ctx->analyzer.emit_gir(ctx->root_mod)};
    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "compute");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("mul i32"));
    CHECK(dump_text.contains("add i32"));
    CHECK(dump_text.contains("ret i32"));
}

} // namespace ghoti::tests
