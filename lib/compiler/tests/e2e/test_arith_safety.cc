#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("signed overflow of +, -, * traps at runtime", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 2000000000;
            var b: i32 = 2000000000;
            return a + b;
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 100000;
            var b: i32 = 100000;
            return a * b;
        };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i8 = 100;
            var b: i8 = 100;
            return @as(i32, a + b);
        };
    )") != 0);
}

TEST_CASE("negating INT_MIN traps", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 2147483647;
            var lo: i32 = -a - 1;   // i32 minimum
            return -lo;
        };
    )") != 0);
}

TEST_CASE("integer division and remainder by zero trap (signed and unsigned)", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { var a: i32 = 5; var b: i32 = 0; return a / b; };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { var a: i32 = 5; var b: i32 = 0; return a % b; };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { var a: u32 = 5u; var b: u32 = 0u; return @as(i32, a / b); };
    )") != 0);
}

TEST_CASE("INT_MIN / -1 traps", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i8 = -128;
            var b: i8 = -1;
            return @as(i32, a / b);
        };
    )") != 0);
}

TEST_CASE("out-of-range shift amount traps", "[.panic]") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { var x: i32 = 1; var s: i32 = 40; return x << s; };
    )") != 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 { var x: i32 = 256; var s: i32 = 64; return x >> s; };
    )") != 0);
}

TEST_CASE("well-formed arithmetic is unaffected by the safety checks") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 100;
            var b: i32 = 7;
            var s: i32 = 3;
            return (a * b) / (b - 2) + (a % b) - (a << s) / 100 - (-b);   // 700/5 + 2 - 8 - (-7)
        };
    )") == (700 / 5 + 2 - 8 + 7));
}

TEST_CASE("unsigned +, -, * still wrap without trapping") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var d: u16 = 65535;
            var e: u16 = 10;
            d = d + e;                 // wraps to 9
            var m: u16 = 30000;
            m = m * 3;                 // wraps
            return @as(i32, d);
        };
    )") == 9);
}

} // namespace ghoti::tests
