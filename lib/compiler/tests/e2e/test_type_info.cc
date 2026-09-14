#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`builtin.TypeKind` values construct and match") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const k: builtin.TypeKind = .int;
            return match (k) {
                .int => 1,
                _ => 0,
            };
        };
    )") == 1);
}

TEST_CASE("`builtin.CallConv` matches `ast::calling_convention`'s spellings") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const cc: builtin.CallConv = .c;
            return match (cc) {
                .c => 1,
                .sysv, .win64, .stdcall, .fastcall, .aapcs => 0,
            };
        };
    )") == 1);
}

TEST_CASE("`builtin.IntInfo` carries a bit width and signedness") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const info: builtin.IntInfo = .{ .bits = 32, .signed = true, .is_constexpr = false };
            return @intCast(i32, info.bits);
        };
    )") == 32);
}

TEST_CASE("`builtin.FloatInfo` carries a bit width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const info: builtin.FloatInfo = .{ .bits = 64, .is_constexpr = false };
            return @intCast(i32, info.bits);
        };
    )") == 64);
}

TEST_CASE("the full `builtin.TypeInfo` union (all 16 arms) resolves cleanly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T: type = builtin.TypeInfo;
            _ = T;
            return 0;
        };
    )") == 0);
}

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

TEST_CASE("a same-named sibling member doesn't shadow an outer type regardless of declaration "
          "order") {
    CHECK(helpers::compile_and_run(R"(
        const Before := union {
            other: void,
            @"void": i32,
        };
        pub const main := fn(): i32 {
            if (@bitSizeOf(@typeOf(Before{ .other = {} }.other)) != @bitSizeOf(void)) { return 1; }
            return 0;
        };
    )") == 0);
    CHECK(helpers::compile_and_run(R"(
        const After := union {
            @"void": i32,
            other: void,
        };
        pub const main := fn(): i32 {
            if (@bitSizeOf(@typeOf(After{ .other = {} }.other)) != @bitSizeOf(void)) { return 1; }
            return 0;
        };
    )") == 0);
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

TEST_CASE("`@typeInfo`'s `IntInfo`/`FloatInfo` mark `comptime_int`/`comptime_float` via "
          "`is_constexpr`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(@typeOf(5))) {
                .int => |i| @intFromBool(i.is_constexpr) * 100 + @intCast(i32, i.bits),
                _ => -1,
            };
        };
    )") == 132);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(i32)) {
                .int => |i| @intFromBool(i.is_constexpr),
                _ => -1,
            };
        };
    )") == 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(@typeOf(5.0))) {
                .float => |f| @intFromBool(f.is_constexpr) * 100 + @intCast(i32, f.bits),
                _ => -1,
            };
        };
    )") == 164);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(f32)) {
                .float => |f| @intFromBool(f.is_constexpr),
                _ => -1,
            };
        };
    )") == 0);
}

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

TEST_CASE("`if constexpr` can fold a compile-time string's `.len`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if constexpr ("hello".len == 5) {
                return 0;
            }
            return 1;
        };
    )") == 0);
}

TEST_CASE("`if constexpr` can fold a compile-time slice range `arr[lo..hi]`") {
    CHECK(helpers::compile_and_run(R"(
        constexpr eql := fn(T: type, a: []T, b: []T): bool {
            if (a.len != b.len) return false;
            for (a, b) |x, y| { if (x != y) return false; }
            return true;
        };
        pub const main := fn(): i32 {
            if constexpr (eql(u8, "hello world"[0..5], "hello")) {
                return 0;
            }
            return 1;
        };
    )") == 0);
}

TEST_CASE("`if constexpr` can compare `@typeInfo(T)` against a bare tagged-union variant "
          "literal") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if constexpr (@typeInfo(u8) != .float) {
                return 0;
            }
            return 1;
        };
    )") == 0);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            if constexpr (@typeInfo(f32) == .float) {
                return 0;
            }
            return 1;
        };
    )") == 0);
}

TEST_CASE("consecutive `for constexpr` iterations that fold `if constexpr` to the SAME branch "
          "don't corrupt an adjacent iteration's own (different) verdict") {
    CHECK(helpers::compile_and_run(R"(
        const Data := struct { count_x: f32, count_y: u32, other: bool };
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for constexpr (@typeInfo(Data).@"struct".fields) |field| {
                if constexpr (field.name.len == 7) {
                    acc += 1;
                }
            }
            return acc;
        };
    )") == 2);
}

TEST_CASE("a bare (non-`match`) `@typeInfo(T).int` field access spilled to memory materializes the "
          "real tagged-union payload") {
    constexpr std::string_view MATH_MOD{R"(
        pub constexpr maxIntBits := fn(T: type): i32 {
            constexpr info := @typeInfo(T).int;
            return @intCast(i32, info.bits);
        };
    )"};
    CHECK(helpers::compile_and_run_tests(
              R"(
            import "sub/math.gh" as math;

            pub constexpr eql := fn(T: type, a: []T, b: []T): bool {
                if (a.len != b.len) return false;
                return true;
            };

            test "dummy" { @expect(eql(u8, "ab", "ab")); }
            test "maxIntBits(u1) reads the real payload" {
                @expect(math.maxIntBits(u1) == 1);
            }
        )",
              {helpers::mock_file{"sub/math.gh", MATH_MOD, "math"}}) == 0);
}

TEST_CASE("`if constexpr` calling a `std.mem`-shaped `startsWith`/`eql` pair folds per-field "
          "inside a `for constexpr` over `@typeInfo(T).fields`") {
    CHECK(helpers::compile_and_run(R"(
        constexpr eql := fn(T: type, a: []T, b: []T): bool {
            if (a.len != b.len) return false;
            if (a.len == 0) return true;
            if (@typeInfo(T) != .float and a.ptr == b.ptr) return true;
            for (a, b) |a_elem, b_elem| {
                if (a_elem != b_elem) return false;
            }
            return true;
        };
        constexpr startsWith := fn(T: type, haystack: []T, needle: []T): bool {
            return if (needle.len > haystack.len) false else eql(T, haystack[0..needle.len], needle);
        };
        const Data := struct { count_x: f32, count_y: u32, other: bool };
        pub const main := fn(): i32 {
            var acc: i32 = 0;
            for constexpr (@typeInfo(Data).@"struct".fields) |field| {
                if constexpr (startsWith(u8, field.name, "count_")) {
                    acc += 1;
                }
            }
            return acc;
        };
    )") == 2);
}

} // namespace ghoti::tests
