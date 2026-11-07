#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view RESULT_PRELUDE = R"(
const Result = fn(comptime T: type, comptime E: type): type { return union(enum) { ok: T, err: E }; };
impl(T: type, E: type) builtin.Unwrappable for Result(T, E) {
    const Output = T;
    const Residual = E;
    pub const branch = fn(&mut? self): builtin.Flow(&mut? T, E) {
        return match (self) {
            .ok => |&mut? v| .{ .@"continue" = v },
            .err => |e| .{ .@"break" = e },
        };
    };
}
impl(T: type, E: type) builtin.Rewrappable for Result(T, E) {
    const From = E;
    pub const from_residual = fn(r: E): @This() { return .{ .err = r }; };
}
)";

} // namespace

TEST_CASE("`A || B` keeps the left enum's values and appends the right's new tags") {
    CHECK(helpers::compile_and_run(R"(
        const Io = enum : i32 { not_found = 1, denied, broken };
        const Own = enum { overflow, broken, bad_spec };
        const Both = Io || Own;
        pub const main = fn(): i32 {
            let mut r: i32 = 0;
            if (@backingInt(Both.not_found) != 1 or @backingInt(Both.broken) != 3) { r |= 1; }
            if (@backingInt(Both.overflow) != 4 or @backingInt(Both.bad_spec) != 5) { r |= 2; }
            if (@typeInfo(Both).@"enum".fields.len != 5) { r |= 4; }
            if (@typeInfo(Both).@"enum".tag_type != i32) { r |= 8; }
            return r;
        };
    )") == 0);
}

TEST_CASE("An enum widens into a merge built from it, keeping each tag by name") {
    CHECK(helpers::compile_and_run(R"(
        const A = enum { a1, a2 };
        const B = enum { b1, b2 };
        const C = enum { c1 };
        const AB = A || B;
        const ABC = AB || C;
        const kind = fn(e: ABC): i32 {
            return match (e) {
                .a1, .a2 => 1,
                .b1, .b2 => 2,
                .c1 => 3,
            };
        };
        pub const main = fn(): i32 {
            let mut k: i32 = 0;
            k += 0;
            let a: A = if (k == 0) .a2 else .a1;
            let b: B = if (k == 0) .b2 else .b1;
            let c: C = .c1;
            let from_a: ABC = a;
            let from_b: ABC = b;
            let from_c: ABC = c;
            let via_ab: AB = b;
            let from_ab: ABC = via_ab;
            let mut r: i32 = 0;
            if (from_a != .a2 or kind(from_a) != 1) { r |= 1; }
            if (from_b != .b2 or kind(from_b) != 2) { r |= 2; }
            if (from_c != .c1 or kind(from_c) != 3) { r |= 4; }
            if (from_ab != .b2) { r |= 8; }
            const folded: ABC = B.b1;
            if (folded != .b1) { r |= 16; }
            return r;
        };
    )") == 0);
}

TEST_CASE("`?` carries an enum residual into a merge built from it") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const Io = enum : i32 { not_found = 1, denied };
        const Own = enum { bad, worse };
        const Merged = Io || Own;
        const io_op = fn(k: i32): Result(i32, Io) {
            return if (k == 0) .{ .err = .denied } else .{ .ok = k };
        };
        const own_op = fn(k: i32): Result(i32, Own) {
            return if (k == 1) .{ .err = .worse } else .{ .ok = k };
        };
        const both = fn(k: i32, seen: ^mut i32): Result(i32, Merged) {
            errdefer |e| *seen = @backingInt(e);
            let a = io_op(k)?;
            let b = own_op(k)?;
            return .{ .ok = a + b };
        };
        pub const main = fn(): i32 {
            let mut k: i32 = 0;
            k += 0;
            let mut seen: i32 = -1;
            let mut r: i32 = 0;
            match (both(k, ^mut seen)) {
                .ok => { r |= 1; },
                .err => |e| { if (e != .denied or seen != 2) { r |= 2; } },
            }
            match (both(k + 1, ^mut seen)) {
                .ok => { r |= 4; },
                .err => |e| { if (e != .worse or seen != 4) { r |= 8; } },
            }
            match (both(k + 2, ^mut seen)) {
                .ok => |v| { if (v != 4) { r |= 16; } },
                .err => { r |= 32; },
            }
            return r;
        };
    )") == 0);
}

TEST_CASE("`A || B` names one enum wherever it is written, including in a signature") {
    CHECK(helpers::compile_and_run(R"(
        const A = enum { a };
        const B = enum { b };
        const pick = fn(k: i32): A || B {
            if (k == 0) { return A.a; }
            return B.b;
        };
        const is_b = fn(e: A || B): bool { return e == .b; };
        const Inline = A || enum { z };
        pub const main = fn(): i32 {
            let mut k: i32 = 0;
            k += 0;
            let mut r: i32 = 0;
            if ((A || B) != (A || B)) { r |= 1; }
            if (!is_b(pick(k + 1)) or is_b(pick(k))) { r |= 2; }
            const z: Inline = .z;
            if (@backingInt(z) != 1) { r |= 4; }
            return r;
        };
    )") == 0);
}

TEST_CASE("`||` reports operands that are not enum types") {
    CHECK(
        helpers::raised("const A = enum { a };\nconst X = i32 || A;", sema::error::TYPE_MISMATCH));
    const auto result{
        helpers::resolve_diags("const f = fn(t: bool): bool { return t || false; };")};
    CHECK(result.message_contains("use 'or' for a logical or"));
}

TEST_CASE("An enum does not widen into a merge it is no part of, nor a merge into its parts") {
    const auto unrelated{helpers::type_check_diags(R"(
        const A = enum { a };
        const B = enum { b };
        const C = enum { a };
        const AB = A || B;
        const f = fn(c: C): AB { return c; };
    )")};
    CHECK(unrelated.message_contains("type 'C' is not assignable to function return type 'AB'"));
    const auto narrowing{helpers::type_check_diags(R"(
        const A = enum { a };
        const B = enum { b };
        const AB = A || B;
        const f = fn(e: AB): A { return e; };
    )")};
    CHECK(narrowing.message_contains("type 'AB' is not assignable to function return type 'A'"));
}

} // namespace ghoti::tests
