#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@intCast round-trip and valid runtime conversions") {
    SECTION("Round-trip usize <-> i32 <-> usize") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var n: usize = 42;
                var m: i32 = @intCast(i32, n);
                var r: usize = @intCast(usize, m);
                return @intCast(i32, r);
            };
        )") == 42);
    }

    SECTION("Sign conversions that fit in range") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var u: u32 = 100;
                var s: i32 = @intCast(i32, u);
                return s;
            };
        )") == 100);
    }

    SECTION("Narrowing conversions that fit in range") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: i64 = 250;
                var b: u8 = @intCast(u8, a);
                return @as(i32, b) - 150;
            };
        )") == 100);
    }

    SECTION("Signed widening into unsigned when non-negative") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var s: i16 = 50;
                var u: u32 = @intCast(u32, s);
                return @intCast(i32, u);
            };
        )") == 50);
    }
}

TEST_CASE("@intCast out-of-range runtime safety traps", "[.panic]") {
    SECTION("Narrowing overflow traps at runtime") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: i64 = 3000000000;
                var b: i32 = @intCast(i32, a);
                return b;
            };
        )") != 0);
    }

    SECTION("Negative value into unsigned traps at runtime") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: i32 = -1;
                var b: u32 = @intCast(u32, a);
                return @intCast(i32, b);
            };
        )") != 0);
    }

    SECTION("Unsigned to signed overflow traps at runtime") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                var a: u32 = 3000000000;
                var b: i32 = @intCast(i32, a);
                return b;
            };
        )") != 0);
    }
}

} // namespace ghoti::tests
