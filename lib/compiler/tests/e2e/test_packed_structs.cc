#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// A non-extern `packed struct` is laid out as a single backing integer whose width
// is the sum of its fields' bit sizes, fields packed LSB-first in declaration order.

TEST_CASE("packed struct is backed by a sum-width integer") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := packed struct { a: u3, b: u5 };
        pub const main := fn(): i32 {
            if (@sizeOf(Pair) != 1) { return 1; }
            var p: Pair = .{ .a = 5, .b = 5 };
            if (@as(i32, p.a) != 5) { return 2; }
            if (@as(i32, p.b) != 5) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed struct fields do not bleed into each other") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := packed struct { a: u3, b: u5 };
        pub const main := fn(): i32 {
            var p: Pair = .{ .a = 7, .b = 0 };
            if (@as(i32, p.a) != 7 or @as(i32, p.b) != 0) { return 1; }
            var q: Pair = .{ .a = 0, .b = 31 };
            if (@as(i32, q.a) != 0 or @as(i32, q.b) != 31) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed struct field write updates only its own bits") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := packed struct { a: u3, b: u5 };
        pub const main := fn(): i32 {
            var p: Pair = .{ .a = 7, .b = 31 };
            p.a = 2;
            if (@as(i32, p.a) != 2 or @as(i32, p.b) != 31) { return 1; }
            p.b = 9;
            if (@as(i32, p.a) != 2 or @as(i32, p.b) != 9) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed struct LSB-first layout is observable through @bitCast") {
    CHECK(helpers::compile_and_run(R"(
        const Pair := packed struct { a: u3, b: u5 };
        pub const main := fn(): i32 {
            const p: Pair = .{ .a = 5, .b = 5 };
            // b occupies bits 3..7: 5 | (5 << 3) == 45
            return @as(i32, @bitCast(u8, p));
        };
    )") == 45);
}

TEST_CASE("packed struct wider than a machine word") {
    CHECK(helpers::compile_and_run(R"(
        const Wide := packed struct { lo: u40, hi: u40 };
        pub const main := fn(): i32 {
            if (@sizeOf(Wide) != 16) { return 1; }
            var w: Wide = .{ .lo = 1000000, .hi = 2000000 };
            if (@as(i32, @as(u64, w.lo)) != 1000000) { return 2; }
            if (@as(i32, @as(u64, w.hi)) != 2000000) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed struct containing bool and enum fields") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum : u2 { Red = 0, Green = 1, Blue = 2 };
        const Flags := packed struct { on: bool, c: Color, n: u5 };
        pub const main := fn(): i32 {
            var f: Flags = .{ .on = true, .c = Color.Blue, .n = 21 };
            if (!f.on) { return 1; }
            if (f.c != Color.Blue) { return 2; }
            if (@as(i32, f.n) != 21) { return 3; }
            f.on = false;
            if (f.on or f.c != Color.Blue or @as(i32, f.n) != 21) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("nested packed struct field round-trips") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := packed struct { x: u4, y: u4 };
        const Outer := packed struct { head: u8, body: Inner };
        pub const main := fn(): i32 {
            if (@sizeOf(Outer) != 2) { return 1; }
            var o: Outer = .{ .head = 200, .body = .{ .x = 3, .y = 12 } };
            if (@as(i32, o.head) != 200) { return 2; }
            if (@as(i32, o.body.x) != 3 or @as(i32, o.body.y) != 12) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("nested packed struct field write updates only its own bits") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := packed struct { x: u4, y: u4 };
        const Outer := packed struct { head: u8, body: Inner };
        pub const main := fn(): i32 {
            var o: Outer = .{ .head = 200, .body = .{ .x = 3, .y = 12 } };
            o.body.x = 9;
            if (@as(i32, o.body.x) != 9) { return 1; }
            if (@as(i32, o.body.y) != 12) { return 2; }
            if (@as(i32, o.head) != 200) { return 3; }
            o.body.y = 1;
            o.head = 5;
            if (@as(i32, o.body.x) != 9 or @as(i32, o.body.y) != 1 or @as(i32, o.head) != 5) {
                return 4;
            }
            return 0;
        };
    )") == 0);
}

TEST_CASE("nested packed struct compound assignment") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := packed struct { x: u4, y: u4 };
        const Outer := packed struct { head: u8, body: Inner };
        pub const main := fn(): i32 {
            var o: Outer = .{ .head = 10, .body = .{ .x = 1, .y = 2 } };
            o.body.x += 5;
            o.body.y *= 3;
            o.head -= 4;
            if (@as(i32, o.body.x) != 6) { return 1; }
            if (@as(i32, o.body.y) != 6) { return 2; }
            if (@as(i32, o.head) != 6) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("three-deep nested packed struct write") {
    CHECK(helpers::compile_and_run(R"(
        const A := packed struct { lo: u2, hi: u2 };
        const B := packed struct { a: A, tag: u4 };
        const C := packed struct { b: B, flag: bool };
        pub const main := fn(): i32 {
            var c: C = .{ .b = .{ .a = .{ .lo = 0, .hi = 0 }, .tag = 0 }, .flag = false };
            c.b.a.hi = 3;
            c.b.tag = 10;
            c.flag = true;
            if (@as(i32, c.b.a.hi) != 3 or @as(i32, c.b.a.lo) != 0) { return 1; }
            if (@as(i32, c.b.tag) != 10) { return 2; }
            if (!c.flag) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("nested packed union write reinterprets the shared bits") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := packed union { a: u8, b: u4 };
        const Outer := packed struct { head: u4, body: Inner };
        pub const main := fn(): i32 {
            var o: Outer = .{ .head = 5, .body = .{ .a = 255 } };
            o.body.b = 1;
            if (@as(i32, o.body.a) != 241) { return 1; }
            if (@as(i32, o.head) != 5) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("extern packed struct keeps C field layout, not bit-packing") {
    CHECK(helpers::compile_and_run(R"(
        const E := extern packed struct { a: i32, b: u8 };
        pub const main := fn(): i32 {
            var e: E = .{ .a = 42, .b = 7 };
            return e.a + @as(i32, e.b);
        };
    )") == 49);
}

// A non-extern `packed union` is a single backing integer wide enough for its largest
// field; every field is laid out at bit offset 0.

TEST_CASE("packed union is sized to its widest field") {
    CHECK(helpers::compile_and_run(R"(
        const U := packed union { a: u8, b: u3 };
        pub const main := fn(): i32 {
            if (@sizeOf(U) != 1) { return 1; }
            var u: U = .{ .a = 200 };
            if (@as(i32, u.a) != 200) { return 2; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed union field write overwrites the shared bits") {
    CHECK(helpers::compile_and_run(R"(
        const U := packed union { a: u8, b: u3 };
        pub const main := fn(): i32 {
            var u: U = .{ .a = 255 };
            u.b = 2;
            // b wrote the low 3 bits; the upper 5 bits of `a` were left untouched by the
            // read-modify-write, so a == 0b11111010 == 250
            if (@as(i32, u.b) != 2) { return 1; }
            if (@as(i32, u.a) != 250) { return 2; }
            u.a = 9;
            if (@as(i32, u.a) != 9 or @as(i32, u.b) != 1) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("packed union with float and int views of the same bits") {
    CHECK(helpers::compile_and_run(R"(
        const U := packed union { bits: u32, f: f32 };
        pub const main := fn(): i32 {
            var u: U = .{ .bits = 0 };
            u.f = 1.0;
            // IEEE-754 single 1.0 == 0x3F800000
            return @as(i32, @as(i64, u.bits) - 1065353216);
        };
    )") == 0);
}

TEST_CASE("extern packed union is not bit-packed") {
    CHECK(helpers::compile_and_run(R"(
        const U := extern packed union { a: i32, b: f32 };
        pub const main := fn(): i32 {
            var u: U = .{ .a = 17 };
            return u.a;
        };
    )") == 17);
}

TEST_CASE("@bitSizeOf reports the backing width of bit-packed aggregates") {
    CHECK(helpers::compile_and_run(R"(
        const S := packed struct { a: u3, b: u5, c: bool };
        const U := packed union { a: u8, b: u20, c: u3 };
        pub const main := fn(): i32 {
            if (@bitSizeOf(S) != 9) { return 1; }
            if (@bitSizeOf(U) != 20) { return 2; }
            // @sizeOf rounds the 9-bit struct up to 2 ABI bytes
            if (@sizeOf(S) != 2) { return 3; }
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
