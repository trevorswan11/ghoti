#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// A same-named sibling member inside an aggregate's own body used to shadow an outer, keyword-
// named TYPE even in a position where only a type can go (found while building `TypeInfo`
// itself, which has arms literally named `bool`/`void`/etc. mirroring `TypeKind` - worked around
// there with a `NoPayload` struct instead of a bare `void`, see builtin.gh.inc). A type-position
// identifier lookup now skips a same-named sibling that's already, concretely resolved to a
// value, walking outward for the actual type instead of stopping at the first same-named symbol
// regardless of kind.
//
// Known residual gap: this only covers a sibling declared BEFORE the arm referencing the outer
// type (already resolved to a value by the time it's looked up, as here). The reverse order -
// referencing the type before its same-named, not-yet-processed sibling is declared - still
// doesn't resolve correctly; it now reports a clean "referenced before its declaration" instead
// of the original silent wrong-poison, which is real progress, but isn't the full fix. Chasing
// that down further needs the resolver to distinguish "a forward-declared TYPE, legitimately not
// processed yet" from "a not-yet-processed VALUE sibling occupying the same name slot" - the
// same single-namespace root cause called out as out of scope when this bug was first found.
TEST_CASE("a same-named sibling member doesn't shadow an outer type once it's itself resolved") {
    CHECK(helpers::compile_and_run(R"(
        const U := union {
            @"void": bool,
            @"first": void,
        };
        pub const main := fn(): i32 {
            var u := U{ .@"void" = true };
            return if (u.@"void") 1 else 0;
        };
    )") == 1);
}

TEST_CASE("`@typeInfo` on integer and float types") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(i32)) {
                .int => |i| @intFromBool(i.signed) + @intCast(i32, i.bits),
                _ => 0,
            };
        };
    )") == 33);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(u8)) {
                .int => |i| (1 - @intFromBool(i.signed)) * @intCast(i32, i.bits),
                _ => 0,
            };
        };
    )") == 8);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(f64)) {
                .float => |f| @intCast(i32, f.bits),
                _ => 0,
            };
        };
    )") == 64);
}

// `isize`/`usize` used to fall to the `.internal` catch-all - `sema::is_integer` already treats
// them as integers everywhere else, this was purely a gap in `eval_type_info`'s own switch.
TEST_CASE("`@typeInfo` on `isize`/`usize` tags as `.int`, not `.internal`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(usize)) {
                .int => |i| (1 - @intFromBool(i.signed)) * @intCast(i32, i.bits),
                _ => -1,
            };
        };
    )") == 64);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(isize)) {
                .int => |i| @intFromBool(i.signed) * 10 + @intCast(i32, i.bits),
                _ => -1,
            };
        };
    )") == 74);
}

TEST_CASE("`@typeInfo` on payload-less kinds tags correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(bool)) {
                .@"bool" => 1,
                _ => 0,
            };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(void)) {
                .@"void" => 1,
                _ => 0,
            };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(type)) {
                .@"type" => 1,
                _ => 0,
            };
        };
    )") == 1);
}

TEST_CASE("`@typeInfo` tags pointer, reference, slice, and array correctly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(^i32)) { .pointer => 1, _ => 0 };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(&i32)) { .reference => 1, _ => 0 };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo([]i32)) { .slice => 1, _ => 0 };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo([5]i32)) {
                .array => |a| @intCast(i32, a.len),
                _ => 0,
            };
        };
    )") == 5);
}

TEST_CASE("`@typeInfo`'s pointer/array `is_mut` reads through a builtin-call argument") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(^mut i32)) {
                .pointer => |p| @intFromBool(p.is_mut),
                _ => -1,
            };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo([5]i32)) {
                .array => |a| @intFromBool(a.is_mut),
                _ => -1,
            };
        };
    )") == 0);
}

TEST_CASE("`@typeInfo` on an enum tags correctly and reads `exhaustive`") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(Color)) {
                .@"enum" => |e| @intFromBool(e.exhaustive),
                _ => -1,
            };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue, _ };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(Color)) {
                .@"enum" => |e| @intFromBool(e.exhaustive),
                _ => -1,
            };
        };
    )") == 0);
}

TEST_CASE("`@typeInfo` on a struct tags correctly and reads its own flags") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(Point)) {
                .@"struct" => |s| @intFromBool(s.is_packed),
                _ => -1,
            };
        };
    )") == 0);
    CHECK(helpers::compile_and_run(R"(
        const P := extern struct { a: i32 };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(P)) {
                .@"struct" => |s| @intFromBool(s.is_extern),
                _ => -1,
            };
        };
    )") == 1);
    CHECK(helpers::compile_and_run(R"(
        const P := packed struct { a: u4, b: u4 };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(P)) {
                .@"struct" => |s| @intCast(i32, s.backing_bits) + @intFromBool(s.is_packed) * 100,
                _ => -1,
            };
        };
    )") == 108);
}

TEST_CASE("`@typeInfo` on a union tags correctly and reads `tagged`") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(U)) {
                .@"union" => |u| @intFromBool(u.tagged),
                _ => -1,
            };
        };
    )") == 1);
}

TEST_CASE("`for constexpr` over a match-captured payload's own slice field now works") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        const use := fn(): i32 {
            return match constexpr (@typeInfo(Color)) {
                .@"enum" => |e| blk: {
                    var count := 0;
                    for constexpr (e.fields) |f| { count = count + 1; _ = f; }
                    break :blk count;
                },
                _ => -1,
            };
        };
        pub const main := fn(): i32 {
            return use();
        };
    )") == 3);
}

TEST_CASE("a match-captured payload's field is readable inside a nested `for constexpr` body") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        const use := fn(): i32 {
            return match constexpr (@typeInfo(Color)) {
                .@"enum" => |e| blk: {
                    var total := 0;
                    for constexpr (e.fields) |f| {
                        total = total + @intCast(i32, f.name.len);
                    }
                    break :blk total;
                },
                _ => -1,
            };
        };
        pub const main := fn(): i32 {
            return use();
        };
    )") == 12); // "red" + "green" + "blue" = 3 + 5 + 4
}

// A `match constexpr` arm's captured payload now gets the same `constexpr_frame` binding a
// `for`/`while constexpr` capture already gets, so a plain runtime `if` on a captured field
// reads it correctly instead of trying (and failing) to materialize the whole payload as
// ordinary storage - `EnumInfo`/`TypeInfo` carry a `type`-kind field in every arm, which has no
// LLVM size at all.
TEST_CASE("a plain runtime `if` on a match-captured payload field reads it correctly") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        const use := fn(): i32 {
            return match constexpr (@typeInfo(Color)) {
                .@"enum" => |e| if (e.exhaustive) 1 else 0,
                _ => -1,
            };
        };
        pub const main := fn(): i32 {
            return use();
        };
    )") == 1);
}

} // namespace ghoti::tests
