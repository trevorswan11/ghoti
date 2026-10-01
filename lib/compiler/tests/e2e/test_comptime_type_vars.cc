#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a `comptime let mut` type names whatever type it holds at each use") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            let x: T = 1;
            T = u64;
            let y: T = 2;
            return @intCast(i32, @sizeOf(@TypeOf(x)) + @sizeOf(@TypeOf(y)));
        };
    )") == 1 + 8);
}

TEST_CASE("a `comptime let mut` type can start without an annotation and change in a branch") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut T = i32;
            if comptime (T == i32) { T = u8; }
            let v: T = 200;
            return @intCast(i32, v) - 199;
        };
    )") == 1);
}

TEST_CASE("a `comptime let mut` type builds up across `for comptime` iterations") {
    CHECK(helpers::compile_and_run(R"(
        const Wrap = fn(T: type): type { return struct { inner: T, pad: u8 }; };
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            for comptime (0..3) |_| { T = Wrap(T); }
            let w: T = undefined;
            _ = w;
            return @intCast(i32, @sizeOf(T));
        };
    )") == 4);
}

TEST_CASE("a `comptime let mut` type builds up across `while comptime` iterations") {
    CHECK(helpers::compile_and_run(R"(
        const Wrap = fn(T: type): type { return struct { inner: T, pad: u8 }; };
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            comptime let mut n: usize = 0;
            while comptime (n < 3) { T = Wrap(T); n += 1; }
            return @intCast(i32, @sizeOf(T));
        };
    )") == 4);
}

TEST_CASE("a `comptime let mut` type changes inside a `comptime` block and a generic") {
    CHECK(helpers::compile_and_run(R"(
        const widest = fn(a: auto, b: auto): usize {
            comptime let mut T: type = @TypeOf(a);
            if comptime (@sizeOf(@TypeOf(b)) > @sizeOf(T)) { T = @TypeOf(b); }
            return @sizeOf(T);
        };
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            comptime { T = u16; }
            let x: u8 = 1;
            let y: i64 = 2;
            return @intCast(i32, @sizeOf(T) + widest(x, y) + widest(x, x));
        };
    )") == 2 + 8 + 1);
}

TEST_CASE("a `comptime let mut` type feeds struct fields and `@typeName`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            T = u32;
            const S = struct { x: T, y: T };
            let s: S = .{ .x = 1, .y = 2 };
            return @intCast(i32, @sizeOf(S)) + @intCast(i32, s.y) +
                   @intCast(i32, @typeName(T).len);
        };
    )") == 8 + 2 + 3);
}

TEST_CASE("only a type can be assigned to a `comptime let mut` type") {
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            T = 5;
            return 0;
        };
    )",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(R"(
        pub const main = fn(): i32 {
            comptime let mut T: type = u8;
            T += u8;
            return 0;
        };
    )",
                          sema::error::TYPE_MISMATCH));
    // A runtime binding still can't hold a type
    CHECK(helpers::raised("pub const main = fn(): i32 { let mut T: type = u8; return 0; };",
                          sema::error::MUTABLE_TYPE_BINDING));
}

} // namespace ghoti::tests
