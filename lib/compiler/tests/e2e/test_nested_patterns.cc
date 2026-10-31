#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("Nested variant patterns reach through payloads") {
    CHECK(helpers::compile_and_run(R"(
        const O = union(enum) { some: i32, none: void };
        const R = union(enum) { ok: O, err: i32 };
        const f = fn(r: R): i32 {
            return match (r) {
                .ok(.some(|raw|)) => raw,
                .ok(.none), .err => 100,
            };
        };
        pub const main = fn(): i32 {
            return f(R{ .ok = O{ .some = 7 } }) + f(R{ .ok = O{ .none = {} } }) + f(R{ .err = 1 });
        };
    )") == 207);
}

TEST_CASE("Nested variants test each constructor on its own path") {
    SECTION("rows sharing a prefix") {
        CHECK(helpers::compile_and_run(R"(
            const B = union(enum) { a: i32, b: i32 };
            const O = union(enum) { x: B, y: B };
            const f = fn(o: O): i32 {
                return match (o) {
                    .x(.a(|v|)) => v,
                    .x(.b(|v|)) => v * 2,
                    .y(.a(|v|)) => v * 3,
                    .y(.b(|v|)) => v * 4,
                };
            };
            pub const main = fn(): i32 {
                return f(O{ .x = B{ .a = 1 } }) + f(O{ .x = B{ .b = 1 } }) +
                       f(O{ .y = B{ .a = 1 } }) + f(O{ .y = B{ .b = 1 } });
            };
        )") == 10);
    }

    SECTION("an earlier wildcard takes every variant it can") {
        CHECK(helpers::compile_and_run(R"(
            const B = union(enum) { a: i32, b: i32 };
            const O = union(enum) { x: B, y: B };
            const f = fn(o: O): i32 {
                return match (o) {
                    .x(_) => 1,
                    .y(.a(_)) => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return f(O{ .x = B{ .b = 0 } }) * 100 + f(O{ .y = B{ .a = 0 } }) * 10 +
                       f(O{ .y = B{ .b = 0 } });
            };
        )") == 123);
    }
}

TEST_CASE("Struct patterns test and bind named fields") {
    const auto* const point{R"(
        const P = struct { pub x: i32, pub y: i32 };
        const f = fn(p: P): i32 {
            return match (p) {
                .{ .x = 0, .y = 0 } => 1,
                .{ .x = 0, .y = |y| } => y,
                .{ .x = |x|, .y = |y| } => x * y,
            };
        };
    )"};

    SECTION("first match wins") {
        CHECK(helpers::compile_and_run(std::string{point} + R"(
            pub const main = fn(): i32 {
                return f(P{ .x = 0, .y = 0 }) + f(P{ .x = 0, .y = 5 }) * 10;
            };
        )") == 51);
    }

    SECTION("the general row binds both fields") {
        CHECK(helpers::compile_and_run(std::string{point} + R"(
            pub const main = fn(): i32 { return f(P{ .x = 3, .y = 4 }); };
        )") == 12);
    }

    SECTION("omitted fields are ignored") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            pub const main = fn(): i32 {
                let p = P{ .x = 2, .y = 99 };
                return match (p) { .{ .x = 2 } => 5, _ => 6 };
            };
        )") == 5);
    }
}

TEST_CASE("Fixed arrays match by position") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(a: [3]i32): i32 {
            return match (a) {
                .{ 0, |b|, _ } => b,
                .{ |x|, _, |z| } => x + z,
            };
        };
        pub const main = fn(): i32 {
            return f([3]i32{ 0, 9, 1 }) + f([3]i32{ 2, 9, 5 }) * 10;
        };
    )") == 79);
}

TEST_CASE("A pattern refines a value and binds the whole of it") {
    CHECK(helpers::compile_and_run(R"(
        const P = struct { pub x: i32, pub y: i32 };
        const f = fn(p: P): i32 {
            return match (p) { .{ .x = 1.. } |w| => w.y, _ => 0 };
        };
        pub const main = fn(): i32 {
            return f(P{ .x = 3, .y = 8 }) + f(P{ .x = 0, .y = 8 });
        };
    )") == 8);
}

TEST_CASE("Alternatives bind the same names") {
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum) { a: i32, b: i32, c: void };
        const f = fn(u: U): i32 {
            return match (u) { .a(|v|), .b(|v|) => v, .c => 0 };
        };
        pub const main = fn(): i32 { return f(U{ .b = 6 }) + f(U{ .c = {} }); };
    )") == 6);
}

TEST_CASE("Alias captures write through nested places") {
    SECTION("a struct field inside a variant") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32, pub y: i32 };
            const U = union(enum) { a: P, b: i32 };
            pub const main = fn(): i32 {
                let mut u = U{ .a = P{ .x = 1, .y = 2 } };
                match (u) {
                    .a(.{ .y = |&mut y| }) => { y = 40; },
                    .b => {},
                };
                return match (u) { .a(.{ .x = |x|, .y = |y| }) => x + y, .b(|v|) => v };
            };
        )") == 41);
    }

    SECTION("an array element through a pointer capture") {
        CHECK(helpers::compile_and_run(R"(
            pub const main = fn(): i32 {
                let mut a = [2]i32{ 1, 2 };
                match (a) {
                    .{ _, |^mut second| } => { *second = 9; },
                };
                return a[1];
            };
        )") == 9);
    }

    SECTION("a read-only view of a temporary") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const make = fn(): P { return P{ .x = 6 }; };
            pub const main = fn(): i32 {
                return match (make()) { .{ .x = |&x| } => x + 1 };
            };
        )") == 7);
    }
}

TEST_CASE("A by-value capture is a copy of the value it matched") {
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum) { a: i32 };
        pub const main = fn(): i32 {
            let mut u = U{ .a = 1 };
            let mut seen: i32 = 0;
            match (u) {
                .a(|v|) => {
                    u = U{ .a = 50 };
                    seen = v;
                },
            };
            return seen;
        };
    )") == 1);
}

TEST_CASE("Patterns look through a reference but not a pointer") {
    SECTION("a reference field") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub v: i32 };
            const W = struct { pub r: &P };
            const f = fn(w: W): i32 {
                return match (w) {
                    .{ .r = .{ .v = 0 } } => 1,
                    .{ .r = .{ .v = |v| } } => v,
                };
            };
            pub const main = fn(): i32 {
                let p = P{ .v = 9 };
                return f(W{ .r = &p });
            };
        )") == 9);
    }

    SECTION("a reference scrutinee") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub v: i32 };
            const f = fn(p: &mut P): i32 {
                match (p) { .{ .v = |&mut v| } => { v = v + 1; } };
                return p.v;
            };
            pub const main = fn(): i32 {
                let mut p = P{ .v = 4 };
                return f(&mut p);
            };
        )") == 5);
    }

    SECTION("a pointer field stays a pointer") {
        CHECK(helpers::compile_and_run(R"(
            const W = struct { pub p: ^i32 };
            const f = fn(w: W): i32 {
                return match (w) {
                    .{ .p = nullptr } => 0,
                    .{ .p = |p| } => *p,
                };
            };
            pub const main = fn(): i32 {
                let v: i32 = 8;
                return f(W{ .p = ^v }) + f(W{ .p = nullptr });
            };
        )") == 8);
    }
}

TEST_CASE("Qualified patterns name the type they match") {
    CHECK(helpers::compile_and_run(R"(
        const S = union(enum) { circle: i32, square: i32 };
        const P = struct { pub a: i32 };
        const g = fn(p: P): i32 { return match (p) { P{ .a = |a| } => a }; };
        const f = fn(s: S): i32 {
            return match (s) { S.circle(|r|) => r, S.square(|w|) => w * 2 };
        };
        pub const main = fn(): i32 { return f(S{ .square = 4 }) + g(P{ .a = 1 }); };
    )") == 9);
}

TEST_CASE("Ranges decide exhaustiveness") {
    SECTION("covering a byte needs no catch-all") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: u8): i32 { return match (x) { 0..=127 => 1, 128..=255 => 2 }; };
            pub const main = fn(): i32 {
                let mut v: u8 = 0;
                v = 200;
                return f(v) * 10 + f(3u8);
            };
        )") == 21);
    }

    SECTION("open ends reach the type's limits") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: i8): i32 { return match (x) { ..0 => 1, 0 => 2, 1.. => 3 }; };
            pub const main = fn(): i32 {
                let mut lo: i8 = 0;
                lo = -128;
                let mut hi: i8 = 0;
                hi = 127;
                return f(lo) * 100 + f(0i8) * 10 + f(hi);
            };
        )") == 123);
    }

    SECTION("partial overlap takes the first arm") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: i32): i32 { return match (x) { 1..5 => 1, 3..8 => 2, _ => 3 }; };
            pub const main = fn(): i32 {
                let mut v: i32 = 0;
                v = 4;
                return f(v) * 100 + f(6) * 10 + f(9);
            };
        )") == 123);
    }

    SECTION("wide integers") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(x: u256): i32 {
                return match (x) {
                    0x10000000000000000000000000000000000 => 1,
                    0..0x10000000000000000000000000000000000 => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                let big: u256 = 0x10000000000000000000000000000000000;
                return f(big) * 10 + f(5);
            };
        )") == 12);
    }
}

TEST_CASE("Top-level runtime patterns stay lazy and in order") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut hits: i32 = 0;
            const probe = fn(h: &mut i32, v: i32): i32 {
                h = h + 1;
                return v;
            };
            let mut n: i32 = 0;
            n = 2;
            let r = match (n) {
                probe(&mut hits, 1) => 10,
                probe(&mut hits, 2) => 20,
                probe(&mut hits, 3) => 30,
                _ => 40,
            };
            return r + hits;
        };
    )") == 22);
}

TEST_CASE("A generic pattern gets its types per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const Box = fn(comptime T: type): type {
            return struct { pub v: T, pub ok: bool };
        };
        const get = fn(comptime T: type, b: Box(T), other: T): T {
            return match (b) {
                .{ .ok = true, .v = |v| } => v,
                .{ .ok = false } => other,
            };
        };
        pub const main = fn(): i32 {
            const small = get(i8, Box(i8){ .v = 1i8, .ok = true }, 2i8);
            const wide = get(i64, Box(i64){ .v = 3i64, .ok = false }, 4i64);
            return @as(i32, small) + @intCast(i32, wide);
        };
    )") == 5);
}

TEST_CASE("Bit-packed fields can be tested and copied") {
    CHECK(helpers::compile_and_run(R"(
        const F = packed struct { pub a: u3, pub b: u5 };
        const f = fn(p: F): i32 {
            return match (p) {
                .{ .a = 0 } => 1,
                .{ .a = |a|, .b = |b| } => @as(i32, a) * 10 + @as(i32, b),
            };
        };
        pub const main = fn(): i32 {
            return f(F{ .a = 2, .b = 7 });
        };
    )") == 27);
}

TEST_CASE("Nested patterns fold at compile time") {
    SECTION("the same match folded and at runtime") {
        CHECK(helpers::compile_and_run(R"(
            const O = union(enum) { some: i32, none: void };
            const P = struct { pub x: i32, pub o: O };
            const f = fn(p: P): i32 {
                return match (p) {
                    .{ .x = 0.., .o = .some(|v|) } => v,
                    _ => 2,
                };
            };
            const K: i32 = f(P{ .x = 1, .o = O{ .some = 5 } });
            pub const main = fn(): i32 {
                let mut q = P{ .x = 1, .o = O{ .some = 5 } };
                q.x = 1;
                return K * 10 + f(q);
            };
        )") == 55);
    }

    SECTION("an alias capture writes a compile-time place") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { pub x: i32 };
            const f = fn(): i32 {
                let mut p = P{ .x = 1 };
                match (p) { .{ .x = |&mut x| } => { x = 8; } };
                return p.x;
            };
            const K: i32 = f();
            pub const main = fn(): i32 { return K; };
        )") == 8);
    }

    SECTION("arrays") {
        CHECK(helpers::compile_and_run(R"(
            const f = fn(a: [2]i32): i32 {
                return match (a) { .{ 1, |b| } => b, _ => 0 };
            };
            const K: i32 = f([2]i32{ 1, 6 });
            pub const main = fn(): i32 { return K; };
        )") == 6);
    }
}

TEST_CASE("match comptime reads nested patterns") {
    SECTION("binding a value from type info") {
        CHECK(helpers::compile_and_run(R"(
            const bits = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .int(.{ .bits = |b| }) => @as(i32, b),
                    _ => 0,
                };
            };
            pub const main = fn(): i32 { return bits(u16) + bits(bool); };
        )") == 16);
    }

    SECTION("testing a value from type info") {
        CHECK(helpers::compile_and_run(R"(
            const kind = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .int(.{ .bits = 8 }) => 1,
                    .int => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return kind(u8) * 100 + kind(u16) * 10 + kind(bool);
            };
        )") == 123);
    }

    SECTION("capturing a type from type info") {
        CHECK(helpers::compile_and_run(R"(
            const size = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .pointer(.{ .child = |C| }) => @intCast(i32, @sizeOf(C)),
                    _ => 0,
                };
            };
            pub const main = fn(): i32 { return size(^u16) + size(bool); };
        )") == 2);
    }

    SECTION("testing a type from type info") {
        CHECK(helpers::compile_and_run(R"(
            const check = fn(comptime T: type): i32 {
                return match comptime (@typeInfo(T)) {
                    .pointer(.{ .child = u8 }) => 1,
                    .pointer => 2,
                    _ => 3,
                };
            };
            pub const main = fn(): i32 {
                return check(^u8) * 100 + check(^u16) * 10 + check(bool);
            };
        )") == 123);
    }
}

} // namespace ghoti::tests

namespace ghoti::tests {

namespace {

constexpr std::string_view SHAPES{R"(
    const O = union(enum) { some: i32, none: void };
    const R = union(enum) { ok: O, err: i32 };
    const P = struct { pub x: i32, pub y: i32 };
    const F = packed struct { pub a: u3, pub b: u5 };
    const r = R{ .err = 0 };
    const p = P{ .x = 0, .y = 0 };
)"};

[[nodiscard]] auto rejects(std::string_view body, sema::error code) -> bool {
    return helpers::raised(fmt::format("{}\npub const main = fn(): i32 {{ {} }};", SHAPES, body),
                           code);
}

} // namespace

TEST_CASE("Nested matches report exactly what they miss") {
    constexpr auto ILLEGAL{sema::error::ILLEGAL_MATCH_PATTERN};
    constexpr auto MISSING{sema::error::MISSING_FIELD};

    CHECK(rejects("return match (r) { .ok(.some(|v|)) => v, .err => 0 };", MISSING));
    CHECK(rejects("return match (p) { .{ .x = 0 } => 1 };", MISSING));
    CHECK(
        rejects("return match (r) { .ok(.none) => 1, .err(0) => 2, .err(1..) => 3, .ok(_) => 4 };",
                MISSING));

    SECTION("an arm an earlier one covers") {
        CHECK(rejects("return match (r) { .ok(_) => 1, .ok(.none) => 2, .err => 3 };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |x| } => x, .{ .x = 0 } => 2 };", ILLEGAL));
        CHECK(rejects("let n: i32 = 1; return match (n) { 0..5 => 1, 3 => 2, _ => 0 };", ILLEGAL));
    }
}

TEST_CASE("Malformed nested patterns are rejected with a reason") {
    constexpr auto ILLEGAL{sema::error::ILLEGAL_MATCH_PATTERN};

    SECTION("variants") {
        CHECK(rejects("return match (r) { .ok |x| => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(_, _) => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok() => 1, .err => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(.none(_)) => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .ok(.nope) => 1, _ => 2 };", sema::error::UNKNOWN_FIELD));
    }

    SECTION("captures") {
        CHECK(rejects("return match (r) { .err(|v|) => |w| v, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (r) { .err(|v|), .ok(.some(|w|)) => 1, _ => 2 };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |v|, .y = |v| } => v };", ILLEGAL));
        CHECK(rejects("let f = F{ .a = 0, .b = 0 }; return match (f) { .{ .a = |&a| } => 1 };",
                      ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = |&mut x| } => x };",
                      sema::error::ASSIGNMENT_TO_CONST));
    }

    SECTION("aggregates") {
        CHECK(rejects("return match (p) { .{ |a|, |b| } => a };", ILLEGAL));
        CHECK(rejects("return match (p) { .{ .x = 0, .x = 1 } => 1, _ => 2 };",
                      sema::error::DUPLICATE_FIELD));
        CHECK(
            rejects("return match (p) { .{ .z = 0 } => 1, _ => 2 };", sema::error::UNKNOWN_FIELD));
        CHECK(rejects("let a = [2]i32{ 1, 2 }; return match (a) { .{ |x| } => x };", ILLEGAL));
        CHECK(rejects("let a = [2]i32{ 1, 2 }; return match (a) { .{ .x = 1 } => 1, _ => 2 };",
                      ILLEGAL));
        CHECK(rejects("let n: i32 = 1; return match (n) { .{ .x = 1 } => 1, _ => 2 };", ILLEGAL));
    }

    SECTION("qualifiers name the matched type") {
        CHECK(rejects("return match (p) { O{ .x = 0 } => 1, _ => 2 };", ILLEGAL));
    }

    SECTION("nested leaves must be compile-time known") {
        CHECK(rejects("let mut n: i32 = 1; n = 2; return match (p) { .{ .x = n } => 1, _ => 2 };",
                      ILLEGAL));
        CHECK(
            rejects("let mut n: i32 = 1; n = 2; return match (p) { .{ .x = 0..n } => 1, _ => 2 };",
                    ILLEGAL));
    }

    SECTION("floats stay unmatchable at every depth") {
        CHECK(rejects("const Q = struct { pub v: f32 }; let q = Q{ .v = 1.0 };"
                      " return match (q) { .{ .v = 1.0 } => 1, _ => 2 };",
                      sema::error::TYPE_MISMATCH));
    }

    SECTION("a struct value has no equality to test") {
        CHECK(rejects("const W = struct { pub p: P }; let w = W{ .p = p };"
                      " return match (w) { .{ .p = p } => 1, _ => 2 };",
                      ILLEGAL));
    }

    SECTION("open ranges need a sized type") {
        CHECK(rejects("return match (1) { 0.. => 1, _ => 2 };", ILLEGAL));
    }
}

TEST_CASE("Private fields stay private to patterns") {
    CHECK_FALSE(helpers::raised(R"(
        const S = struct { hidden: i32 };
        pub const main = fn(): i32 {
            let s = S{ .hidden = 1 };
            return match (s) { .{ .hidden = |h| } => h };
        };
    )",
                                sema::error::ILLEGAL_PRIVATE_ACCESS));
}

} // namespace ghoti::tests
