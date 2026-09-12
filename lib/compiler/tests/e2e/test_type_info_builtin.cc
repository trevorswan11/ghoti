#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// Phase 9: `@typeInfo(T)` for scalars, pointer/reference/slice/array, and enum. `@typeInfo`'s
// own construction is C++-side (`const_eval::eval_type_info` builds a `gir::const_value`
// directly, never touching a ghoti struct-literal AST node), so it does not hit §11 bug #3
// itself. Its RESULT, though, is a `TypeInfo` *union* - and some arm's payload (pointer/
// reference/slice/array/struct/union/enum/function) always carries a `type`-kind field, so the
// union AS A WHOLE cannot be materialized as an ordinary runtime value, no matter which arm is
// actually active. `resolve_constexpr_match` now gives a `match constexpr` capture the same
// `constexpr_frame` binding a `for`/`while constexpr` capture already got (§5.3/§6.2), so a
// captured payload field is reachable via `try_eval` from anywhere that resolves by name - not
// just a read that happens to be a direct builtin-call argument (`emit_call`'s fold-first
// fallback, the only path that worked before this fix) - including `for constexpr` over the
// payload's own slice field (`e.fields`, `builtin::EnumInfo`'s flagship consumer, Phase 15's real
// prerequisite). A plain runtime `if`/assignment on a payload field is still a known limitation -
// see the "known limitation" test near the bottom - since those try to materialize the whole
// enclosing union as an ordinary value rather than staying inside `const_eval`. Always call
// `@typeInfo` inline inside `match constexpr` (never through an intermediate `const`/`constexpr`
// binding either - a local non-scalar constexpr binding's value isn't reachable by `try_eval` on
// a later bare reference to it, the same pre-existing limitation documented for `for constexpr`'s
// array domain in the design doc §5.2).
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

// FIXED: `match constexpr`'s captured payload (`|e|`) now gets the same `constexpr_frame`
// binding a `for`/`while constexpr` capture already got (§5.3/§6.2) - `resolve_constexpr_match`
// binds it (the active union variant's own payload, mirroring `const_eval::eval_match`'s own
// extraction) for the duration of resolving the live arm, so a *later* `try_eval` on any
// expression referencing it by name can now resolve it - not just a read that happens to be a
// direct builtin-call argument (the one path `emit_call`'s fold-first fallback already covered).
// This unblocks `std::meta`'s flagship `for constexpr (@typeInfo(T).fields) |f| {...}`
// field-walker (Phase 15's real prerequisite - see the design doc §9.1/§14).
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

// Still a known limitation: the `constexpr_frame` fix above only makes the capture reachable by
// *name* through `try_eval` (what `for`/`while constexpr` and a builtin-call argument both use).
// A plain runtime `if`/assignment on a payload field instead tries to materialize the *whole*
// enclosing `TypeInfo` union as an ordinary value first - the union is still unsized (some arm
// always carries a `type`-kind field), so this silently falls through to the wrong branch rather
// than reading the real field. Not attempted here; would need the same "keep it inside
// const_eval" treatment `for constexpr`'s own body already gets, extended to plain `if`.
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
