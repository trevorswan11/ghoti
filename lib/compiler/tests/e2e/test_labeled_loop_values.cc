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
            return found;
        };
    )") == (6 * 100 + 7));
}

} // namespace ghoti::tests
