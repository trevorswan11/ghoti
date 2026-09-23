#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: assigning an array to a constant-bounded range copies it in") {
    const auto exit_code{helpers::compile_and_run(R"(
        const digits2 := fn(v: u8): [2]u8 { return [2]u8{ '0' + v / 10, '0' + v % 10 }; };

        pub const main := fn(): i32 {
            var buf: [6]mut u8 = .{ 0, 0, 0, 0, 0, 0 };
            var index: usize = buf.len;
            index -= 2;
            buf[index..][0..2] = digits2(42);
            index -= 2;
            buf[index..][0..2] = digits2(17);
            const d := fn(c: u8): i32 { return @as(i32, c - '0'); };
            const r: i32 = d(buf[2]) * 1000 + d(buf[3]) * 100 + d(buf[4]) * 10 + d(buf[5]);
            return r - 1742 + @as(i32, buf[0]);
        };
    )")};

    CHECK(exit_code == 0);
}

TEST_CASE("E2E: a known-length slice source is copied with overlap-safe semantics") {
    const auto exit_code{helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [6]mut u8 = .{ 1, 2, 3, 4, 5, 6 };
            a[0..3] = a[2..5];
            const r: i32 = @as(i32, a[0]) * 100 + @as(i32, a[1]) * 10 + @as(i32, a[2]);
            return r - 300;
        };
    )")};

    CHECK(exit_code == 45);
}

TEST_CASE("E2E: a range copy accepts `.{...}` sources and `*slice` destinations") {
    const auto exit_code{helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [4]mut u8 = .{ 1, 2, 3, 4 };
            a[2..] = .{ 7, 8 };
            const b: [2]u8 = .{ 9, 6 };
            *a[0..2] = b;
            const r: i32 = @as(i32, a[0]) * 1000 + @as(i32, a[1]) * 100 + @as(i32, a[2]) * 10 +
                           @as(i32, a[3]);
            return r - 9600;
        };
    )")};

    CHECK(exit_code == 78);
}

TEST_CASE("E2E: assigning a slice variable still rebinds it rather than copying") {
    const auto exit_code{helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: [4]mut u8 = .{ 1, 2, 3, 4 };
            var b: [4]mut u8 = .{ 5, 6, 7, 8 };
            var s: []mut u8 = a[0..2];
            s = b[0..3];
            s[0] = 50;
            const r: i32 = @intCast(i32, s.len) * 10 + @as(i32, a[0]) + @as(i32, b[0]);
            return r;
        };
    )")};

    CHECK(exit_code == 30 + 1 + 50);
}

TEST_CASE("E2E: range copies and element writes through a range fold at compile time") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr build := fn(): [4]u8 {
            var buf: [4]mut u8 = .{ 0, 0, 0, 0 };
            buf[1..3] = [2]u8{ 5, 6 };
            buf[2..][1] = 9;
            return buf;
        };
        constexpr x := build();

        pub const main := fn(): i32 {
            const r: i32 = @as(i32, x[0]) * 1000 + @as(i32, x[1]) * 100 + @as(i32, x[2]) * 10 +
                           @as(i32, x[3]);
            return r - 500;
        };
    )")};

    CHECK(exit_code == 69);
}

} // namespace ghoti::tests
