#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Runs `t()` both at runtime and folded through `comptime t()`; each must give `expected`
auto check_both(std::string_view defs, u32 expected) -> void {
    const auto runtime{fmt::format("{}\npub const main = fn(): i32 {{ return t(); }};", defs)};
    const auto folded{
        fmt::format("{}\npub const main = fn(): i32 {{ return comptime t(); }};", defs)};
    CHECK(helpers::compile_and_run(runtime) == expected);
    CHECK(helpers::compile_and_run(folded) == expected);
}

} // namespace

TEST_CASE("a local `&mut` writes through to its referent at compile time") {
    check_both(R"(
        const t = fn(): i32 { let mut x: i32 = 4; let p = &mut x; p = 8; return x; };
    )",
               8);
    check_both(R"(
        const t = fn(): i32 { let mut x: i32 = 4; let p = &mut x; p += 3; return x + p; };
    )",
               14);
}

TEST_CASE("a `^mut` pointer writes through to its pointee at compile time") {
    check_both(R"(
        const t = fn(): i32 { let mut x: i32 = 4; let p = ^mut x; *p = 11; return x; };
    )",
               11);
}

TEST_CASE("a `&mut` parameter writes into the caller's binding at compile time") {
    check_both(R"(
        const bump = fn(p: &mut i32): void { p += 5; };
        const t = fn(): i32 { let mut x: i32 = 4; bump(&mut x); bump(&mut x); return x; };
    )",
               14);
}

TEST_CASE("a `&mut` reference reaches a nested field and element at compile time") {
    check_both(R"(
        const S = struct { x: i32, arr: [3]mut i32 };
        const t = fn(): i32 {
            let mut s: S = .{ .x = 1, .arr = .{1, 2, 3} };
            let p = &mut s.arr[1];
            p = 20;
            let q = ^mut s;
            q.x = 7;
            return s.x + s.arr[1];
        };
    )",
               27);
}

TEST_CASE("a `match` capture by mutable reference writes into the union payload") {
    check_both(R"(
        const U = union { a: i32, b: void };
        const t = fn(): i32 {
            let mut u: U = .{ .a = 4 };
            match (u) { .a => |&mut x| { x = 9; }, .b => {} }
            return match (u) { .a => |x| x, .b => 0 };
        };
    )",
               9);
}

TEST_CASE("`for` captures by mutable reference or pointer write into the array") {
    check_both(R"(
        const t = fn(): i32 {
            let a = [_]mut i32{1, 2, 3};
            for (a) |&mut e| { e *= 2; }
            return a[0] + a[1] + a[2];
        };
    )",
               12);
    check_both(R"(
        const t = fn(): i32 {
            let a = [_]mut i32{1, 2, 3};
            for (a) |^mut e| { *e += 1; }
            return a[0] + a[1] + a[2];
        };
    )",
               9);
}

TEST_CASE("a slice of an array views its storage at compile time") {
    check_both(R"(
        const fill = fn(s: []mut i32): void { s[0] = 40; };
        const t = fn(): i32 { let a = [_]mut i32{1, 2, 3}; fill(a); return a[0]; };
    )",
               40);
    check_both(R"(
        const t = fn(): i32 {
            let a = [_]mut i32{1, 2, 3};
            let s: []mut i32 = a[1..];
            s[0] = 50;
            let inner: []mut i32 = s[1..];
            inner[0] = 60;
            return a[1] + a[2] + @intCast(i32, s.len);
        };
    )",
               112);
}

TEST_CASE("a `&mut self` method called through a reference mutates the receiver") {
    check_both(R"(
        const P = struct { v: i32, pub const set = fn(&mut self, n: i32): void { self.v = n; }; };
        const t = fn(): i32 {
            let mut p: P = .{ .v = 1 };
            p.set(33);
            let r = &mut p;
            r.set(r.v + 1);
            r.v += 1;
            return p.v;
        };
    )",
               35);
}

TEST_CASE("a reference parameter returned from a call still names the caller's place") {
    check_both(R"(
        const first = fn(a: &mut [2]mut i32): &mut i32 { return &mut a[0]; };
        const t = fn(): i32 {
            let mut a = [_]mut i32{1, 2};
            let p = first(&mut a);
            p = 30;
            return a[0];
        };
    )",
               30);
}

TEST_CASE("`&mut` of a runtime `let mut` doesn't fold to a copy of its initializer") {
    CHECK(helpers::compile_and_run(R"(
        const U = union { a: i32, b: void };
        const payload = fn(u: &mut U): &mut i32 {
            return match (u) { .a => |&mut v| v, .b => unreachable };
        };
        pub const main = fn(): i32 {
            let mut u: U = .{ .a = 1 };
            let p = payload(&mut u);
            p = 41;
            return match (u) { .a => |v| v + 1, .b => 0 };
        };
    )") == 42);
}

TEST_CASE("a compile-time reference to a callee's own local is an error") {
    helpers::expect_compile_error(R"(
        const leak = fn(): &i32 { let x: i32 = 1; return &x; };
        const read = fn(): i32 { let p = leak(); return p; };
        const VALUE = read();
        pub const main = fn(): i32 { return VALUE; };
    )");
}

TEST_CASE("compile-time pointers step through array elements") {
    check_both(R"(
        const t = fn(): i32 {
            let mut a = [3]mut i32{ 1, 2, 3 };
            let p: ^mut i32 = @ptrFromArray(a);
            p[2] = 4;
            let q = ^mut a[1];
            q[1] += 1;
            return a[0] + a[1] + a[2] + p[1] * 10;
        };
    )",
               28);
}

} // namespace ghoti::tests
