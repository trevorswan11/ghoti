#include <string_view>

#include <fmt/format.h>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`auto` under pointer, reference, slice, and array levels binds the innermost type") {
    CHECK(helpers::compile_and_run(R"(
        const sz = fn(x: &auto): i32 { return @intCast(@sizeOf(@TypeOf(*x))); };
        const bump = fn(x: &mut auto): void { *x += 1; };
        const first = fn(xs: []auto): i32 { return @intCast(xs[0]); };
        const setf = fn(xs: []mut auto): void { xs[0] = 7; };
        const sum3 = fn(xs: [3]auto): i32 { return @intCast(xs[0] + xs[1] + xs[2]); };
        const viaref = fn(x: &[]auto): i32 { return @intCast(x.len); };
        const deref2 = fn(p: ^^auto): i32 { return @intCast(**p); };
        pub const main = fn(): i32 {
            let a: i64 = 3;
            let mut b: i16 = 4;
            bump(&mut b);
            let mut arr: [3]mut u8 = .{ 1, 2, 3 };
            setf(arr[..]);
            let s: []u8 = arr[..];
            let mb = &mut b;
            let p: ^i64 = ^a;
            let pp = ^p;
            return sz(&a) + sz(mb) + @as(i32, b) + first(arr[..]) + sum3(arr) + viaref(&s) +
                   deref2(pp);
        };
    )") == 8 + 2 + 5 + 7 + (7 + 2 + 3) + 3 + 3);
}

TEST_CASE("`mut?` under `auto` takes the argument's own mutability") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(x: &mut? auto): i32 { return @intCast(*x); };
        pub const main = fn(): i32 {
            let a: i64 = 3;
            let mut b: i64 = 2;
            return f(&a) + f(&mut b);
        };
    )") == 5);
}

TEST_CASE("an argument that doesn't match an `auto` pattern is an error at the call") {
    const auto mismatch{
        [](std::string_view src) { CHECK(helpers::raised(src, sema::error::TYPE_MISMATCH)); }};
    mismatch(R"(
        const f = fn(x: &auto): i32 { return 1; };
        pub const main = fn(): i32 { let a: i64 = 3; return f(a); };
    )");
    mismatch(R"(
        const f = fn(x: &mut auto): i32 { return 1; };
        pub const main = fn(): i32 { let a: i64 = 3; return f(&a); };
    )");
    mismatch(R"(
        const f = fn(x: ^auto): i32 { return 1; };
        pub const main = fn(): i32 { let a: i64 = 3; return f(&a); };
    )");
    mismatch(R"(
        const f = fn(x: []mut auto): i32 { return 1; };
        pub const main = fn(): i32 { let a: [2]i64 = .{ 1, 2 }; return f(a[..]); };
    )");
    mismatch(R"(
        const f = fn(xs: [3]auto): i32 { return 0; };
        pub const main = fn(): i32 { let a: [2]u8 = .{ 1, 2 }; return f(a); };
    )");
    // `auto` inside a slice or array is only a parameter pattern
    CHECK(helpers::raised("let x: []auto = undefined;", sema::error::ILLEGAL_AUTO_USAGE));
}

TEST_CASE("`impl I` under a pattern checks its bound on the innermost type") {
    constexpr auto shapes{R"(
        const Area = interface { pub const area = fn(&self): i32; };
        const Sq = struct { s: i32 };
        impl Area for Sq { pub const area = fn(&self): i32 { return self.s * self.s; }; }
        const total = fn(xs: []impl Area): i32 {
            let mut t: i32 = 0;
            for (xs) |x| { t += x.area(); }
            return t;
        };
        const one = fn(x: &impl Area): i32 { return x.area(); };
    )"};
    CHECK(helpers::compile_and_run(fmt::format(R"({}
        pub const main = fn(): i32 {{
            let qs: [2]Sq = .{{ .{{ .s = 2 }}, .{{ .s = 3 }} }};
            let q = Sq{{ .s = 4 }};
            return total(qs[..]) + one(&q);
        }};
    )",
                                               shapes)) == 4 + 9 + 16);
    CHECK(helpers::raised(fmt::format(R"({}
        pub const main = fn(): i32 {{ let q: i32 = 4; return one(&q); }};
    )",
                                      shapes),
                          sema::error::UNSATISFIED_BOUND));
}

TEST_CASE("a plain `auto` parameter takes a reference argument as a reference") {
    CHECK(helpers::compile_and_run(R"(
        const f = fn(x: auto): i32 { return @intCast(*x); };
        pub const main = fn(): i32 { let a: i64 = 3; return f(&a); };
    )") == 3);
}

} // namespace ghoti::tests
