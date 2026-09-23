#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: `*slice` copies a constant-bounded range out as an array") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr digits2 := fn(value: u8): [2]u8 {
            constexpr lookup := "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
            return *lookup[value * 2..][0..2];
        };

        pub const main := fn(): i32 {
            var v: u8 = 40;
            v += 3;
            const a := digits2(v);
            const b := digits2(17);
            const r: i32 = (a[0] - '0') * 10 + (a[1] - '0') + (b[0] - '0') * 10 + (b[1] - '0');
            return r;
        };
    )")};

    CHECK(exit_code == 43 + 17);
}

TEST_CASE("E2E: `*slice` sees the length through a `const` binding") {
    const auto exit_code{helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const arr: [5]u8 = .{ 1, 2, 3, 4, 5 };
            var i: usize = 1;
            i += 1;
            const s := arr[i..][0..3];
            const c := *s;
            const r: i32 = c[0] + c[1] + c[2];
            return r + @intCast(i32, c.len);
        };
    )")};

    CHECK(exit_code == 3 + 4 + 5 + 3);
}

TEST_CASE("E2E: `*slice` with an open end over an array is a mutable copy") {
    const auto exit_code{helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr: [5]mut u8 = .{ 1, 2, 3, 4, 5 };
            var m: [3]mut u8 = *arr[2..];
            m[0] = 10;
            const r: i32 = m[0] + m[2] + arr[2];
            return r;
        };
    )")};

    CHECK(exit_code == 10 + 5 + 3);
}

TEST_CASE("E2E: `*slice` folds at compile time") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr pick := fn(): [2]u8 {
            constexpr s := "hello";
            return *s[1..3];
        };
        constexpr x := pick();

        pub const main := fn(): i32 {
            const r: i32 = x[0] + x[1];
            return r - 200;
        };
    )")};

    CHECK(exit_code == 'e' + 'l' - 200);
}

} // namespace ghoti::tests
