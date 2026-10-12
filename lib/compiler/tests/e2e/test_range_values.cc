#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a range used as a value is the array of values it takes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let x = 0..10;
            let z = 1..=3;
            if (x.len != 10 or x[9] != 9) { return 1; }
            if (z.len != 3 or z[0] != 1 or z[2] != 3) { return 2; }
            let mut s: usize = 0;
            for (x) |v| { s += v; }
            return @intCast(s);
        };
    )") == 45);
}

TEST_CASE("a range value counts down when its start is past its end") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let d = 3..0;
            let n = -1..=-3;
            if (d.len != 3 or d[0] != 3 or d[2] != 1) { return 1; }
            if (n.len != 3 or n[0] != -1 or n[2] != -3) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("a range value takes its element type from the expected type") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(xs: [8]usize): usize { let mut s: usize = 0; for (xs) |x| { s += x; } return s; };
        const sum_u8 = fn(xs: []u8): u32 { let mut s: u32 = 0; for (xs) |x| { s += x; } return s; };
        pub const main = fn(): i32 {
            let y: [4]u8 = 2..6;
            let mut m: [3]mut i16 = 0..3;
            m[1] = 7;
            if (y[0] != 2 or y[3] != 5) { return 1; }
            if (m[1] != 7) { return 2; }
            return @intCast(sum(0..8) + @as(usize, sum_u8(10..=12)));
        };
    )") == 28 + 33);
    // An untyped negative bound counts in `isize`
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { const n = -2..2; const v: isize = n[0]; return @intCast(v + 10); };
    )") == 8);
}

TEST_CASE("a range value's bounds may be constants and comptime parameters") {
    CHECK(helpers::compile_and_run(R"(
        const N = 4;
        const TABLE = N..N * 2;
        const f = fn(comptime n: usize): usize {
            let a = 0..n;
            let mut s: usize = 0;
            for (a) |x| { s += x; }
            return s + a.len;
        };
        const g = fn(T: type, comptime k: T): T { const r = k..=0; return r[0] + @as(T, @intCast(r.len)); };
        pub const main = fn(): i32 {
            if (TABLE[3] != 7) { return 1; }
            return @intCast(f(4) + f(3) + @as(usize, g(u8, 5)));
        };
    )") == 27);
}

TEST_CASE("a range value needs bounds known at compile time that fit its element type") {
    CHECK(helpers::raised("const f = fn(i: usize): usize { let a = i..i + 4; return a.len; };",
                          sema::error::ILLEGAL_OPEN_RANGE));
    CHECK(helpers::raised("const f = fn(): void { let a: [3]u8 = 254..257; _ = a; };",
                          sema::error::LITERAL_OUT_OF_RANGE));
    CHECK(helpers::raised("const f = fn(): void { let a = 0..; _ = a; };",
                          sema::error::ILLEGAL_OPEN_RANGE));
    CHECK(helpers::raised("const f = fn(): void { let a = 0..u8; _ = a; };",
                          sema::error::TYPE_USED_AS_VALUE));
    // The length is part of its type
    helpers::expect_compile_error(
        "pub const main = fn(): i32 { let a: [4]usize = 0..3; _ = a; return 0; };");
}

} // namespace ghoti::tests
