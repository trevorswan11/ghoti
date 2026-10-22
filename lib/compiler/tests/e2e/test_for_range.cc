#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a closed range for-loop sums its half-open interval") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut acc: i32 = 0;
            for (0..5) |i| { acc = acc + i; }
            return acc;
        };
    )") == 0 + 1 + 2 + 3 + 4);
}

TEST_CASE("a range for-loop endpoint may be a negative literal, and the capture is signed") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut acc: i32 = 0;
            for (-2..3) |i| { acc = acc + i; }
            return acc;
        };
    )") == -2 + -1 + 0 + 1 + 2);
}

TEST_CASE("`..` binds looser than arithmetic in a range endpoint") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 2;
            let mut acc: i32 = 0;
            for (n - 1..n * 3) |i| { acc = acc + i; }
            return acc;
        };
    )") == 1 + 2 + 3 + 4 + 5);
}

TEST_CASE("`arr[0..arr.len - 1]` groups the subtraction inside the range") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(s: []i32): i32 {
            let mut a: i32 = 0;
            for (s) |v| { a = a + v; }
            return a;
        };
        pub const main = fn(): i32 {
            let mut arr = [5uz]mut i32{1, 2, 3, 4, 100};
            return sum(arr[0uz..arr.len - 1uz]);
        };
    )") == 1 + 2 + 3 + 4);
}

TEST_CASE("`for (arr, 0..) |v, i|` enumerates: the sibling array bounds the open range") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut arr = [4uz]mut i32{10, 20, 30, 40};
            let mut acc: i32 = 0;
            for (arr, 0..) |v, i| { acc = acc + v + @intCast(i32, i); }
            return acc;
        };
    )") == (10 + 20 + 30 + 40) + (0 + 1 + 2 + 3));
}

TEST_CASE("an open-ended range for-loop with nothing to bound it is rejected") {
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 {
            let mut acc: i32 = 0;
            for (0..) |i| { acc = acc + i; if (i > 3) { break; } }
            return acc;
        };
    )");
}

TEST_CASE("a range over untyped bounds counts in `usize` and converts where every value fits") {
    CHECK(helpers::compile_and_run(R"(
        const take8 = fn(x: u8): i32 { return @as(i32, x); };
        pub const main = fn(): i32 {
            let a: [4]i32 = .{ 1, 2, 3, 4 };
            let mut s: i32 = 0;
            for (0..3) |i| { s += a[i] + a[i + 1]; s += i; }
            for (0..4) |i| { s += take8(i); }
            for (a, 0..) |x, i| { let k: i32 = i; s += k * x; }
            let n: i32 = 2;
            for (0..3) |i| { if (i < n) { s += 100; } }
            return s;
        };
    )") == (6 + 9 + 3) + 6 + 20 + 200);

    // A counter that can outgrow the target keeps its own type
    helpers::expect_compile_error(R"(
        const take8 = fn(x: u8): i32 { return @as(i32, x); };
        pub const main = fn(): i32 { let mut s: i32 = 0; for (0..300) |i| { s += take8(i); } return s; };
    )");
    helpers::expect_compile_error(R"(
        pub const main = fn(): i32 { for (0..3) |i| { let x = i; return x; } return 0; };
    )");
}

TEST_CASE("a range counter sized by a `comptime` bound converts per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(comptime n: usize): i32 {
            let mut s: i32 = 0;
            for (0..n) |i| { let b: u8 = i; s += @as(i32, b); }
            return s;
        };
        pub const main = fn(): i32 { return f(4) + f(3); };
    )") == 6 + 3);
}

TEST_CASE("a range counter can be matched on, addressed, but not assigned") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut s: i32 = 0;
            for (0..3) |i| {
                s += match (i) { 0 => 10, 1 => 20, _ => 30 };
                let p = &i;
                s += @intCast(*p);
            }
            return s;
        };
    )") == 60 + 3);
    helpers::expect_compile_error(
        "pub const main = fn(): i32 { for (0..3) |i| { i = 4; } return 0; };");
}

TEST_CASE("a range whose start is past its end counts down") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut s: i32 = 0;
            for (3..0) |i| { s = s * 10 + @as(i32, @intCast(i)); }
            let lo: i32 = 9;
            let hi: i32 = 7;
            for (lo..hi) |i| { s = s * 10 + i; }
            for (hi..=lo) |i| { s = s * 10 + i; }
            return if (s == 32198789) 7 else 1;
        };
    )") == 7);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut s: i32 = 0;
            for (-1..=-8) |i| { s += @as(i32, @intCast(i)); }
            return -s;
        };
    )") == 36);

    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut n: i32 = 0;
            let k: i32 = 4;
            for (k..k) |_| { n += 1; }
            for (k..=k) |_| { n += 10; }
            return n;
        };
    )") == 10);
}

TEST_CASE("a descending range reaches its type's extremes") {
    CHECK(helpers::compile_and_run(R"(
        const count = fn(lo: u8, hi: u8): i32 {
            let mut n: i32 = 0;
            for (lo..=hi) |_| { n += 1; }
            return n;
        };
        pub const main = fn(): i32 {
            let mut m: i32 = 0;
            for (127i8..=-128) |_| { m += 1; }
            return (count(255, 0) - 256) + (m - 256);
        };
    )") == 0);
}

TEST_CASE("a descending range folds at compile time and unrolls") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(a: i32, b: i32): i32 { let mut s: i32 = 0; for (a..=b) |i| { s += i; } return s; };
        const K = @call(.compile_time, sum, 5, 2);
        pub const main = fn(): i32 {
            let mut u: i32 = 0;
            unroll for (4..1) |i| { u = u * 10 + i; }
            return if (K == 14 and u == 432) 9 else 1;
        };
    )") == 9);
}

TEST_CASE("a descending range counter converts when its values fit") {
    CHECK(helpers::compile_and_run(R"(
        const take = fn(x: u8): i32 { return x; };
        pub const main = fn(): i32 {
            let mut s: i32 = 0;
            for (200..=198) |i| { s += take(i); }
            return s - 500;
        };
    )") == 97);
}

TEST_CASE("a subscript range cannot count down") {
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 { let a: [5]i32 = .{1, 2, 3, 4, 5}; const s = a[3..1]; return 0; };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 { let a: [5]i32 = .{1, 2, 3, 4, 5}; const s = a[2..=1]; return @intCast(s.len); };
    )") == 0);
}

TEST_CASE("an inclusive range stops at its type's maximum") {
    CHECK(helpers::compile_and_run(R"(
        const count = fn(lo: u8, hi: u8): i32 {
            let mut n: i32 = 0;
            for (lo..=hi) |_| { n += 1; if (n > 50) { return 99; } }
            return n;
        };
        const AT_COMPILE_TIME = count(250, 255);
        pub const main = fn(): i32 { return count(250, 255) * 10 + AT_COMPILE_TIME; };
    )") == 6 * 10 + 6);
}

TEST_CASE("an inclusive range folded at compile time includes its end") {
    CHECK(helpers::compile_and_run(R"(
        const sum = fn(n: i32): i32 { let mut s: i32 = 0; for (0..=n) |i| { s += i; } return s; };
        const K = sum(3);
        pub const main = fn(): i32 { return K; };
    )") == 6);
}

} // namespace ghoti::tests
