#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("GIR match literal patterns and value yield") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_match := fn(x: i32): i32 {
            return match (x) {
                1 => 10,
                2 => 20,
                _ => 30,
            };
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "test_match");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("eq bool"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR match enum patterns") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Color := enum {
            RED,
            GREEN,
            BLUE,
        };

        const test_match_enum := fn(c: Color): i32 {
            return match (c) {
                .RED => 1,
                .GREEN => 2,
                .BLUE => 3,
            };
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

    CHECK(dump_text.contains("eq bool"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR match arm capture") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const UnionVal := union {
            int_val: i32,
            bool_val: bool,
        };

        const test_match_capture := fn(u: UnionVal): i32 {
            return match (u) {
                .int_val => |val| val + 10,
                _ => 0,
            };
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

    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR struct initialization and field access") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct {
            x: i32,
            y: i32,
        };

        const test_struct := fn(): i32 {
            var p: Point = Point{ .x = 10, .y = 20 };
            return p.x + p.y;
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    REQUIRE(gir_mod.get_functions().size() == 1);
    const auto& fn{*gir_mod.get_functions()[0]};
    CHECK(fn.get_name() == "test_struct");

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("alloca"));
    CHECK(dump_text.contains("get_element_ptr"));
    CHECK(dump_text.contains("store"));
    CHECK(dump_text.contains("load"));
    CHECK(dump_text.contains("add i32"));
}

TEST_CASE("GIR struct field assignment and compound assignment") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Point := struct {
            x: i32,
            y: i32,
        };

        const test_mutate := fn(): i32 {
            var p: Point = .{ .x = 1, .y = 2 };
            p.x = 42;
            p.y += 10;
            return p.x + p.y;
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

    CHECK(dump_text.contains("get_element_ptr"));
    CHECK(dump_text.contains("store"));
    CHECK(dump_text.contains("add i32"));
}

TEST_CASE("GIR array index expression and assignment") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_array := fn(arr: [4]i32, i: usize): i32 {
            return arr[i];
        };
    )")};

    gir::emitter emitter{ctx->analyzer.get_ctx(), ctx->root_mod};
    const auto   gir_mod{emitter.emit()};

    // `test_array` plus the `weak` `panic_handler` pulled in for the `arr[i]` bounds check.
    REQUIRE(gir_mod.get_functions().size() == 2);
    const auto& fn{*gir_mod.get_functions()[0]};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(fn);
    const auto dump_text{ss.view()};

    CHECK(dump_text.contains("get_element_ptr"));
    CHECK(dump_text.contains("load"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR address_of and dereference") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_ptr := fn(x: i32): i32 {
            var a: i32 = x;
            const p: ^i32 = ^a;
            *p = 99;
            return *p;
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

    CHECK(dump_text.contains("address_of"));
    CHECK(dump_text.contains("store"));
    CHECK(dump_text.contains("load"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR defer at block exit with LIFO execution") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_defer := fn(): i32 {
            var a: i32 = 0;
            {
                defer a = a * 2;
                defer a = a + 10;
                a = 1;
            }
            return a;
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

    // First defer (+10) then second defer (*2) must appear in that order before return
    const auto plus_pos{dump_text.find("add i32")};
    const auto mul_pos{dump_text.find("mul i32")};
    CHECK(plus_pos != std::string::npos);
    CHECK(mul_pos != std::string::npos);
    CHECK(plus_pos < mul_pos);
}

TEST_CASE("GIR defer at early return triggers unwinding") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_defer_return := fn(x: i32): i32 {
            var a: i32 = 0;
            defer a = 100;
            if (x > 0) {
                return 1;
            }
            return 2;
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

    // Defer store (100) must be present in return paths
    CHECK(dump_text.contains("store 100"));
}

TEST_CASE("GIR defer at loop break and continue unwinding") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_defer_loop := fn(): i32 {
            var sum: i32 = 0;
            var i: i32 = 0;
            while (i < 10) {
                defer i = i + 1;
                if (i == 5) {
                    break;
                }
                if (i == 2) {
                    continue;
                }
                sum = sum + i;
            }
            return sum;
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

    // Defer expression (i = i + 1) must be emitted across break, continue, and loop fallthrough
    CHECK(dump_text.contains("add i32"));
    CHECK(dump_text.contains("goto"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR match on boolean patterns") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const test_match_bool := fn(flag: bool): i32 {
            return match (flag) {
                true => 1,
                false => 0,
            };
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

    CHECK(dump_text.contains("eq bool"));
    CHECK(dump_text.contains("cond_goto"));
    CHECK(dump_text.contains("ret"));
}

TEST_CASE("GIR structs with nested struct field access") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Inner := struct {
            val: i32,
        };

        const Outer := struct {
            inner: Inner,
            scale: i32,
        };

        const test_nested := fn(): i32 {
            var o: Outer = Outer{
                .inner = Inner{ .val = 7 },
                .scale = 6,
            };
            return o.inner.val * o.scale;
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

    CHECK(dump_text.contains("get_element_ptr"));
    CHECK(dump_text.contains("mul i32"));
    CHECK(dump_text.contains("ret"));
}

} // namespace ghoti::tests
