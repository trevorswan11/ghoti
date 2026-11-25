#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

using sema::error;

TEST_CASE("Cast builtins fold at compile time", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const E = enum : u8 { a = 1, b = 4 };
        const a: u8 = @intCast(300 - 100);
        const b = @truncate(u8, @as(u16, 0x1234));
        const c = @truncate(i8, @as(u16, 0xff));
        const d = @bitCast(u32, @as(f32, 1.5));
        const f = @intFromFloat(i32, @as(f64, -3.75));
        const i = @intFromBool(u8, true);
        const l = @backingInt(E.b);
        const n = @bitCast(i32, @as(u32, 0xffffffff));
        const k = @boolFromInt(@as(u8, 3));
    )")};
    CHECK(run.int_value("a") == 200);
    CHECK(run.int_value("b") == 0x34);
    CHECK(run.int_value("c") == -1);
    CHECK(run.int_value("d") == 0x3fc00000);
    CHECK(run.int_value("f") == -3);
    CHECK(run.int_value("i") == 1);
    CHECK(run.int_value("l") == 4);
    CHECK(run.int_value("n") == -1);
    CHECK(run.gir().contains("const k: bool = true"));
}

TEST_CASE("A one-argument cast takes the type it is used as", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const narrow = fn(x: i32): u8 {
            let a: u8 = @intCast(x);
            return a;
        };
        pub const flag = fn(b: bool): u32 { return @intFromBool(b); };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("int_cast u8 param.0"));
    CHECK(gir.contains("int_cast u32 param.0"));
    CHECK(gir.contains("integer value out of range for target type"));
}

TEST_CASE("A one-argument cast with nothing to take a type from is reported", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        pub const f = fn(x: i32): void {
            let a = @intCast(x);
        };
    )")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(run.messages().front() ==
          "cannot infer the target type of '@intCast' here; write '@intCast(T, x)'");
}

TEST_CASE("A known integer out of a cast's range is reported", "[uir]") {
    const auto run{helpers::uir_rejected("const a = @intCast(u8, 300);")};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK(run.messages().front() ==
          "Integer value 300 is out of range for target type 'u8' in @intCast");
}

TEST_CASE("A runtime cast of a known operand keeps the old emitter's shape", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const half = fn(x: f64): f64 { return x / 2.0; };
        pub const f = fn(): i32 {
            return @intCast(i32, @sizeOf(u64)) + @intFromFloat(i32, half(9.0));
        };
    )")};
    const auto gir{run.gir()};

    // The integer cast stays a cast of a constant, with no range check
    CHECK(gir.contains("int_cast i32 8"));
    CHECK_FALSE(gir.contains("integer value out of range"));

    // The float conversion folds, the call it read still made
    CHECK(gir.contains("call @half(9)"));
    CHECK_FALSE(gir.contains("widen_cast.checked i32"));
}

TEST_CASE("A range used as a value is the array of the values it takes", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const down: [3]usize = 3..0;
        const typed: [4]u8 = 2..6;
        pub const first: i32 = down[0];
        pub const count: i32 = down.len;
        pub const last: i32 = typed[3];
    )")};
    CHECK(run.int_value("first") == 3);
    CHECK(run.int_value("count") == 3);
    CHECK(run.int_value("last") == 5);
}

TEST_CASE("A range a for iterates is known whatever its other iterables lower to", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        pub const f = fn(): i32 {
            let arr: [3]i32 = .{ 1, 1, 1 };
            let mut sum: i32 = 0;
            for (0..3, blk: {
                for (0..3) |_| {}
                break :blk arr;
            }) |_, v| { sum += v; }
            return sum;
        };
    )")};
    CHECK(run.gir().contains("fn f"));
}

TEST_CASE("An integer cast to an enum checks it lands on a variant", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const E = enum : u8 { a = 1, b = 4 };
        pub const f = fn(x: u8): E { return @fromBackingInt(E, x); };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("bit_cast enum : u8 param.0"));
    CHECK(gir.contains("invalid enum value"));
}

} // namespace ghoti::tests
