#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: a local `constexpr` array of `type`s is compile-time only") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "t" {
            constexpr ts := [_]type{ i32, u8 };
            @expect(ts.len == 2);
            @expect(ts[0] == i32);
            @expect(ts[1] == u8);
            @expect(@sizeOf(ts[0]) == 4);
        }
    )") == 0);
}

TEST_CASE("E2E: a local `const` array of `type`s is compile-time only") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "t" {
            const ts := [_]type{ i32, u8, u64 };
            @expect(ts.len == 3);
            @expect(ts[2] == u64);
            @expect(ts[0] != ts[1]);
        }
    )") == 0);
}

TEST_CASE("E2E: an element of a `type` array names a usable type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr ts := [_]type{ i32, u8 };
            constexpr Small := ts[1];
            const x: Small = 40;
            return @intCast(i32, x) + @intCast(i32, @sizeOf(ts[0]));
        };
    )") == 44);
}

TEST_CASE("E2E: a module-level `type` array folds at every use") {
    CHECK(helpers::compile_and_run_tests(R"(
        const ts := [_]type{ u8, u16, u32 };
        constexpr more := [_]type{ i64 };

        const size_at := fn(constexpr i: usize): usize { return @sizeOf(ts[i]); };

        test "t" {
            @expect(ts.len == 3);
            @expect(size_at(2) == 4);
            @expect(@sizeOf(more[0]) == 8);
        }
    )") == 0);
}

TEST_CASE("E2E: `for constexpr` walks a `type` array") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr ts := [_]type{ u8, u16, u32, u64 };
            var total: usize = 0;
            for constexpr (ts) |T| { total += @sizeOf(T); }
            return @intCast(i32, total);
        };
    )") == 15);
}

TEST_CASE("E2E: nested arrays and slices of `type`s are compile-time only") {
    CHECK(helpers::compile_and_run_tests(R"(
        test "t" {
            constexpr grid := [_][2]type{ .{ i8, i16 }, .{ i32, i64 } };
            @expect(grid.len == 2);
            @expect(grid[1].len == 2);
            @expect(grid[1][0] == i32);

            constexpr ts := [_]type{ bool, u8, f32 };
            constexpr tail := ts[1..3];
            @expect(tail.len == 2);
            @expect(tail[1] == f32);
        }
    )") == 0);
}

TEST_CASE("E2E: a struct value holding a `type` field is compile-time only") {
    CHECK(helpers::compile_and_run(R"(
        const Slot := struct { T: type, count: usize };

        pub const main := fn(): i32 {
            constexpr slot := Slot{ .T = u16, .count = 3 };
            const x: slot.T = 7;
            return @intCast(i32, @sizeOf(slot.T) * slot.count) + @intCast(i32, x);
        };
    )") == 13);
}

TEST_CASE("E2E: a `constexpr var` array of `type`s can be rewritten at compile time") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var ts := [_]type{ u8, u8 };
            ts[1] = u64;
            return @intCast(i32, @sizeOf(ts[0]) + @sizeOf(ts[1]));
        };
    )") == 9);
}

TEST_CASE("E2E: a generic signature's placeholder target never emits parameterized impl bodies") {
    // `empty`'s own `Res([]mut T, Oops)` instantiates `Res` over the still-unbound `T`
    CHECK(helpers::compile_and_run(R"(
        const Res := fn(T: type, E: type): type {
            return union { ok: T, err: E };
        };

        impl(T: type, E: type) builtin.Unwrappable for Res(T, E) {
            const Output := T;
            const Residual := E;
            pub const branch := fn(self): builtin.Flow(T, E) {
                return match (self) {
                    .ok => |v| builtin.Flow(T, E){ .@"continue" = v },
                    .err => |e| builtin.Flow(T, E){ .@"break" = e },
                };
            };
        }

        impl(T: type, E: type) builtin.Rewrappable for Res(T, E) {
            const From := E;
            pub const from_residual := fn(r: E): @This() {
                return .{ .err = r };
            };
        }

        const Oops := enum { bad };

        const empty := fn(T: type): Res([]mut T, Oops) {
            return .{ .err = .bad };
        };

        const pick := fn(): Res(i32, Oops) { return .{ .ok = 2 }; };
        const add := fn(): Res(i32, Oops) {
            const v := pick()?;
            return .{ .ok = v + 1 };
        };

        pub const main := fn(): i32 {
            const miss: i32 = match (empty(u8)) { .ok => 100, .err => 0 };
            return miss + match (add()) { .ok => |v| v, .err => 50 };
        };
    )") == 3);
}

} // namespace ghoti::tests
