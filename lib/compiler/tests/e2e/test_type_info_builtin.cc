#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

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

TEST_CASE("known limitation: a plain runtime `if` on a match-captured payload field still "
          "misreads it") {
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
    )") == 0); // should be 1 (`Color` IS exhaustive) once this limitation is fixed
}

} // namespace ghoti::tests
