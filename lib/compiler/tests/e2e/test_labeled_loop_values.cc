#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("labeled `loop` as a decl initializer yields the break value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var i: i32 = 0;
            const r := outer: loop {
                i += 1;
                if (i == 5) { break :outer i * 10; }
            };
            return r;
        };
    )") == 50);
}

TEST_CASE("labeled `loop` in `return` position yields the break value") {
    CHECK(helpers::compile_and_run(R"(
        const first_multiple := fn(n: i32): i32 {
            var i: i32 = 1;
            return scan: loop {
                if (i * n > 20) { break :scan i * n; }
                i += 1;
            };
        };

        pub const main := fn(): i32 {
            return first_multiple(7);  // 7,14,21 -> 21
        };
    )") == 21);
}

TEST_CASE("labeled block as a decl initializer yields the break value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const r := blk: {
                const a := 40;
                const b := 2;
                break :blk a + b;
            };
            return r;
        };
    )") == 42);
}

TEST_CASE("labeled loop yielding a struct value") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := struct { a: i32, b: i32 };

        pub const main := fn(): i32 {
            var i: i32 = 0;
            const p := outer: loop {
                i += 1;
                if (i == 3) { break :outer Pair{ .a = 30, .b = 12 }; }
            };
            return p.a + p.b;
        };
    )") == 42);
}

TEST_CASE("`defer` in an enclosing scope runs exactly once when `return`ing a labeled block") {
    CHECK(helpers::compile_and_run(R"(
        const run := fn(log: ^mut i32): i32 {
            defer *log = *log + 1;
            return blk: {
                var acc: i32 = 0;
                acc += 1;
                break :blk acc;
            };
        };
        pub const main := fn(): i32 {
            var log: i32 = 0;
            _ = run(^mut log);
            return log;
        };
    )") == 1);
}

TEST_CASE("`defer` in an enclosing scope runs exactly once when breaking a labeled loop used as "
          "a `return` value") {
    CHECK(helpers::compile_and_run(R"(
        const run := fn(log: ^mut i32): i32 {
            defer *log = *log + 1;
            var i: i32 = 0;
            return outer: loop {
                i += 1;
                if (i == 3) { break :outer i * 10; }
            };
        };
        pub const main := fn(): i32 {
            var log: i32 = 0;
            const v := run(^mut log);
            return v + log * 10;
        };
    )") == 40);
}

TEST_CASE("nested labeled loops break to the outer label with a value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 0;
            const found := outer: loop {
                x += 1;
                var y: i32 = 0;
                inner: loop {
                    y += 1;
                    if (x * y == 42) { break :outer x * 100 + y; }
                    if (y == 10) { break :inner; }
                };
                if (x == 10) { break :outer 0 - 1; }
            };
            return found - 600;
        };
    )") == 7);
}

TEST_CASE("labeled block values are re-typed per generic instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const widen := fn(value: auto): u64 {
            const a := blk: {
                break :blk value;
            };
            return @intCast(u64, a);
        };
        const magnitude := fn(value: auto): u64 {
            constexpr info := @typeInfo(@TypeOf(value)).int;
            using Unsigned = @Int(.{ .signedness = .unsigned, .bits = info.bits });
            const a: Unsigned = blk: {
                if constexpr (info.signedness == .signed) {
                    const bits := @bitCast(Unsigned, value);
                    break :blk if (value < 0) 0 -% bits else bits;
                } else {
                    break :blk value;
                }
            };
            return @intCast(u64, a);
        };
        pub const main := fn(): i32 {
            const w := widen(1u8) + widen(20u32) + widen(300u64);
            const m := magnitude(-5i8) + magnitude(40u16) + magnitude(-500i64);
            return @intCast(i32, w + m) - 800;
        };
    )") == 66);
}

TEST_CASE("labeled loop values are re-typed per generic instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const first_over := fn(limit: auto): u64 {
            var i: @TypeOf(limit) = 0;
            const found := search: loop {
                i += 1;
                if (i > limit) { break :search i; }
            };
            return @intCast(u64, found);
        };
        pub const main := fn(): i32 {
            return @intCast(i32, first_over(3u8) + first_over(1000u32)) - 1000;
        };
    )") == 5);
}

} // namespace ghoti::tests
