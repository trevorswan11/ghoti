#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// Phase 9: `@typeInfo(T)` for scalars, pointer/reference/slice/array, and enum. `@typeInfo`'s
// own construction is C++-side (`const_eval::eval_type_info` builds a `gir::const_value`
// directly, never touching a ghoti struct-literal AST node), so it does not hit §11 bug #3
// itself. Its RESULT, though, is a `TypeInfo` *union* - and some arm's payload (pointer/
// reference/slice/array/struct/union/enum/function) always carries a `type`-kind field, so the
// union AS A WHOLE cannot be materialized, no matter which arm is actually active. Consequence:
// reading ANY field off a `match constexpr`-captured payload only works when the read itself
// stays inside `const_eval` - which happens today only as a direct argument to another builtin
// call (`emit_call`'s fallback for a builtin it doesn't special-case tries `const_eval::try_eval`
// on the whole call before emitting anything ordinary). Plain `if`, `if constexpr`, and
// `for constexpr` over a captured field all instead try to materialize the enclosing union first
// and crash (`Ty->isSized()` in LLVM's `DataLayout`) - see the "known limitation" test at the
// bottom. Always call `@typeInfo` inline inside `match constexpr` (never through an intermediate
// `const`/`constexpr` binding either - a local non-scalar constexpr binding's value isn't
// reachable by `try_eval` on a later bare reference to it, the same pre-existing limitation
// documented for `for constexpr`'s array domain in the design doc §5.2).
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

// Phase 10: `struct`/`union` arms (`function` waits on Phase 13's `callconv`-into-type-identity
// fix - `types::function` has nowhere to read a calling convention from yet).
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

// Known limitation, not attempted here: `match constexpr`'s captured payload (`|e|`) is not
// `const_eval`-foldable at *resolve* time the way a plain `constexpr` binding is - only a
// narrower *emit*-time fallback (an unhandled builtin call tries `const_eval::try_eval` before
// falling back to ordinary emission) makes a payload field reachable at all, and only when
// nothing else forces real materialization. This means neither `if`/`if constexpr` on a payload
// field nor `for constexpr` over a payload's own slice field (`e.fields`, `builtin::EnumInfo`'s
// flagship `for constexpr` + `@typeInfo` consumer) work today. Fixing it is its own unit of
// work - likely giving a match arm's capture the same kind of `constexpr_frame` binding
// `for`/`while constexpr` captures already get (§5.3/§6.2) - deferred rather than attempted here.
TEST_CASE("known limitation: a match-captured payload's own slice field isn't yet consumable") {
    helpers::expect_compile_error(R"(
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
    )");
}

} // namespace ghoti::tests
