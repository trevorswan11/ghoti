#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: a local `const` alias of a type constructor call is a type, not a runtime call") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := fn(T: type): type { return struct { a: T, b: T }; };

        pub const main := fn(): i32 {
            const P := Pair(i32);
            const x := P{ .a = 1, .b = 2 };
            const y: P = .{ .a = 10, .b = 20 };
            var z: P = undefined;
            z.b = 100;
            return x.b + y.b + z.b;
        };
    )") == 122);
}

TEST_CASE("E2E: a local `constexpr` alias of a type constructor call is a type") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := fn(T: type): type { return struct { a: T, b: T }; };

        pub const main := fn(): i32 {
            constexpr P := Pair(i32);
            const x := P{ .a = 1, .b = 2 };
            return x.a + x.b;
        };
    )") == 3);
}

TEST_CASE("E2E: pointer and slice types built over a local constructor alias") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { v: T }; };

        pub const main := fn(): i32 {
            const B := Box(i32);
            const PB := ^mut B;
            var b: B = .{ .v = 1 };
            const p: PB = ^mut b;
            p.v = 41;
            const bs := [2]B{ b, .{ .v = 1 } };
            const s: []B = bs[0..2];
            return s[0].v + s[1].v;
        };
    )") == 42);
}

TEST_CASE("E2E: a constructor alias inside a generic body re-resolves per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { v: T }; };

        const wrap := fn(T: type, val: T): i32 {
            const B := Box(T);
            const b := B{ .v = val };
            return @intCast(i32, @sizeOf(B)) + @intCast(i32, b.v);
        };

        pub const main := fn(): i32 {
            return wrap(u8, 3u8) + wrap(i64, 30i64);
        };
    )") == 1 + 3 + 8 + 30);
}

TEST_CASE("E2E: `Result.map_err`-shaped constexpr alias in a generic method (#334)") {
    CHECK(helpers::compile_and_run(R"(
        const Result := fn(T: type, E: type): type {
            return union {
                ok: T,
                err: E,

                pub constexpr map_err := fn(&self, func: auto): auto {
                    constexpr fn_info := @typeInfo(@TypeOf(func));
                    @assert(fn_info == .function, "map_err requires a function argument");
                    constexpr NewErr := fn_info.function.return_type;
                    constexpr NewRes := Result(T, NewErr);

                    return match (self) {
                        .ok => |happy| NewRes{ .ok = happy },
                        .err => |sad| NewRes{ .err = func(sad) },
                    };
                };
            };
        };

        const widen := fn(e: u8): i32 { return @intCast(i32, e) * 2; };

        pub const main := fn(): i32 {
            const a: Result(i32, u8) = .{ .err = 20 };
            const b: Result(i32, u8) = .{ .ok = 2 };
            const ma := a.map_err(widen);
            const mb := b.map_err(widen);
            var acc: i32 = 0;
            match (ma) { .ok => |v| { acc += v; }, .err => |e| { acc += e; } }
            match (mb) { .ok => |v| { acc += v; }, .err => |e| { acc += e; } }
            return acc;
        };
    )") == 42);
}

TEST_CASE("E2E: `@This()` and `@Int(...)` aliases bind types") {
    CHECK(helpers::compile_and_run(R"(
        const Counter := struct {
            n: i32,

            const Self := @This();
            pub const bump := fn(s: Self): Self { return .{ .n = s.n + 1 }; };
        };

        pub const main := fn(): i32 {
            const U7 := @Int(.{ .signedness = .unsigned, .bits = 7 });
            const x: U7 = 40;
            const c := Counter.bump(.{ .n = 1 });
            return @intCast(i32, x) + c.n;
        };
    )") == 42);
}

TEST_CASE("E2E: module-scope aliases of `void` and of a constructor call are storageless") {
    CHECK(helpers::compile_and_run(R"(
        const V := void;
        const Pair := fn(T: type): type { return struct { a: T }; };
        const P := Pair(i32);

        const nothing := fn(): V {};

        pub const main := fn(): i32 {
            nothing();
            const p: P = .{ .a = 7 };
            return p.a;
        };
    )") == 7);
}

TEST_CASE("E2E: a generic returning its `T: type` parameter's value is still a value") {
    CHECK(helpers::compile_and_run(R"(
        const dup := fn(T: type, val: T): T { return val + val; };

        pub const main := fn(): i32 {
            const a := dup(i32, 20);
            const b := dup(u8, 1u8);
            return a + @intCast(i32, b);
        };
    )") == 42);
}

TEST_CASE("E2E: `@sizeOf(Ctor(T))` in a generic body folds per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Box := fn(T: type): type { return struct { v: T }; };

        const size := fn(T: type): i32 { return @intCast(i32, @sizeOf(Box(T))); };
        const size_cx := fn(T: type): i32 {
            constexpr n := @sizeOf(Box(T));
            return @intCast(i32, n);
        };

        pub const main := fn(): i32 {
            return size(i64) + size(u8) * 10 + size_cx(i64) * 10 + size_cx(u8) * 100;
        };
    )") == 8 + 10 + 80 + 100);
}

} // namespace ghoti::tests
