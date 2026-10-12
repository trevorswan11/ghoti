#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("an `if` computes a type in a type position") {
    CHECK(helpers::compile_and_run(R"(
        const WIDE = true;
        const widen = fn(x: if (WIDE) i64 else i32): if (WIDE) i64 else i32 { return x * 2; };
        pub const main = fn(): i32 {
            let v: if (WIDE) i64 else i32 = widen(21);
            return @intCast(v + @as(i64, @sizeOf(@TypeOf(v))));
        };
    )") == 42 + 8);
}

TEST_CASE("a `match` computes a type from a comptime parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Kind = enum { small, large };
        const pick = fn(comptime k: Kind, x: match (k) { .small => u8, .large => u32 }): usize {
            _ = x;
            return @sizeOf(@TypeOf(x));
        };
        pub const main = fn(): i32 { return @intCast(pick(.small, 1) * 10 + pick(.large, 2)); };
    )") == 14);
}

TEST_CASE("a type expression may read a parameter, which becomes compile-time") {
    CHECK(helpers::compile_and_run(R"(
        const K = enum { a, b };
        const f = fn(comptime k: K): match (k) { .a => u8, .b => u64 } { return 7; };
        const g = fn(k: K, x: match (k) { .a => u8, .b => u64 }): usize { _ = x; return @sizeOf(@TypeOf(x)); };
        const h = fn(wide: bool): if (wide) i64 else i32 { return 3; };
        pub const main = fn(): i32 {
            if (@sizeOf(@TypeOf(f(.a))) != 1 or @sizeOf(@TypeOf(f(.b))) != 8) { return 1; }
            if (g(.a, 1) != 1 or g(.b, 2) != 8) { return 2; }
            return @intCast(@sizeOf(@TypeOf(h(true))) * 10 + @sizeOf(@TypeOf(h(false))));
        };
    )") == 84);
    // Each instantiation checks its arguments against the type it computes
    helpers::expect_compile_error(R"(
        const K = enum { a, b };
        const f = fn(comptime k: K, x: match (k) { .a => u8, .b => u64 }): usize { return x; };
        pub const main = fn(): i32 { return @intCast(f(.a, 300)); };
    )");
}

TEST_CASE("an `if` or `match` whose values are all types is a type value") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { a: i32 };
        const U = struct { b: i32 };
        const K = enum { a, b };
        const T = if (false) U else S;
        const M = match (K.b) { .a => u8, .b => []u8 };
        pub const main = fn(): i32 {
            const L = if (true) S else u8;
            let s: T = .{ .a = 3 };
            let m: M = "hi";
            let l = L{ .a = 4 };
            return s.a + @as(i32, @intCast(m.len)) + l.a;
        };
    )") == 9);
}

TEST_CASE("a labeled block and an index compute a type") {
    CHECK(helpers::compile_and_run(R"(
        const TYPES = [2]type{ u16, i64 };
        pub const main = fn(): i32 {
            let a: blk: { break :blk u8; } = 200;
            let b: TYPES[0] = 300;
            return @intCast(@as(u32, a) + b - 450 + @as(u32, @sizeOf(@TypeOf(b))));
        };
    )") == 52);
}

TEST_CASE("a type expression takes modifiers and nests in other types") {
    CHECK(helpers::compile_and_run(R"(
        const BIG = false;
        pub const main = fn(): i32 {
            let x: i32 = 7;
            let p: &if (BIG) i64 else i32 = &x;
            let xs: [2]if (BIG) i64 else i32 = .{ 1, 2 };
            return *p + xs[1];
        };
    )") == 9);
}

TEST_CASE("a type expression's `{` and `=` end it") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { a: i32 };
        const make = fn(): if (true) S else void { return S{ .a = 5 }; };
        pub const main = fn(): i32 {
            const T = if (false) u8 else S;
            let s: if (true) T else u8 = T{ .a = 4 };
            return make().a + s.a;
        };
    )") == 9);
}

TEST_CASE("every branch of a type expression is checked unless it is comptime") {
    CHECK(helpers::raised("const f = fn(x: if (true) i32 else Missing): void { _ = x; };",
                          sema::error::UNDECLARED_IDENTIFIER));
    CHECK(helpers::compile_and_run(R"(
        const f = fn(x: if comptime (true) i32 else Missing): i32 { return x; };
        pub const main = fn(): i32 { return f(6); };
    )") == 6);
}

TEST_CASE("a type expression must compute a type known at compile time") {
    CHECK(helpers::raised("const f = fn(x: if (true) 1 else 2): void { _ = x; };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised(
        "const f = fn(): void { let mut c = true; c = !c; let x: if (c) i32 else u8 = 0; _ = x; };",
        sema::error::COMPTIME_EVALUATION_FAILED));
}

} // namespace ghoti::tests
