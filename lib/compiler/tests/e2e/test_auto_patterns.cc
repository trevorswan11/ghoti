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

TEST_CASE("`impl Fn(...)` takes any callable with exactly that signature") {
    CHECK(helpers::compile_and_run(R"(
        const apply = fn(g: impl Fn(n: i32): i32, v: i32): i32 { return g(v); };
        const call = fn(g: impl Fn(n: i32): auto, v: i32): auto { return g(v); };
        const holds = fn(g: impl Fn(x: auto): bool, v: auto): bool { return g(v); };
        const dbl = fn(n: i32): i32 { return n * 2; };
        const wide = fn(n: i32): i64 { return @as(i64, n) * 3; };
        const pos = fn(x: i64): bool { return x > 0; };
        pub const main = fn(): i32 {
            let k: i32 = 5;
            let erased: fn(n: i32): i32 = dbl;
            let a = apply(dbl, 4) + apply(fn(n: i32): i32 { return n + k; }, 1) +
                    apply(erased, 1) + apply(^dbl, 1);
            let b: i64 = call(wide, 2);
            let c: i32 = if (holds(pos, @as(i64, 3))) 1 else 0;
            return a + @as(i32, @intCast(b)) + c;
        };
    )") == 8 + 6 + 2 + 2 + 6 + 1);
}

TEST_CASE("a callable that doesn't match `impl Fn(...)` is an error at the call") {
    constexpr auto apply{R"(
        const apply = fn(g: impl Fn(n: i32): i32, v: i32): i32 { return g(v); };
        const wide = fn(n: i64): i32 { return 1; };
        const two = fn(a: i32, b: i32): i32 { return 1; };
        const ret64 = fn(n: i32): i64 { return 1; };
    )"};
    for (const auto* call : {"apply(wide, 1)", "apply(two, 1)", "apply(ret64, 1)", "apply(5, 1)"}) {
        CHECK(helpers::raised(
            fmt::format("{}\npub const main = fn(): i32 {{ return {}; }};", apply, call),
            sema::error::UNSATISFIED_BOUND));
    }
}

TEST_CASE("`impl` bounds hold for callers in other modules and inside other generics") {
    const std::vector<helpers::mock_file> lib{helpers::mock_file{"lib.gh",
                                                                 R"(
        pub const Area = interface { pub const area = fn(&self): i32; };
        pub const one = fn(x: &impl Area): i32 { return 1; };
        pub const apply = fn(g: impl Fn(n: i32): i32, v: i32): i32 { return g(v); };
    )",
                                                                 "lib"}};
    CHECK(helpers::raised(R"(
        import "lib.gh" as lib;
        pub const main = fn(): i32 { let q: i32 = 4; return lib.one(&q); };
    )",
                          sema::error::UNSATISFIED_BOUND,
                          lib));
    CHECK(helpers::raised(R"(
        import "lib.gh" as lib;
        const bad = fn(n: i64): i32 { return 1; };
        pub const main = fn(): i32 { return lib.apply(bad, 1); };
    )",
                          sema::error::UNSATISFIED_BOUND,
                          lib));
    CHECK(helpers::raised(R"(
        const Area = interface { pub const area = fn(&self): i32; };
        const one = fn(x: &impl Area): i32 { return 1; };
        const wrap = fn(y: auto): i32 { return one(&y); };
        pub const main = fn(): i32 { return wrap(4); };
    )",
                          sema::error::UNSATISFIED_BOUND));
}

TEST_CASE("an `impl Fn` member of a type constructor checks each instantiation's signature") {
    constexpr auto boxed{R"(
        const Box = fn(T: type): type {
            return struct {
                v: T,
                pub const map = fn(&self, f: impl Fn(x: T): auto): auto { return f(self.v); };
            };
        };
        const neg = fn(x: i32): i32 { return -x; };
        const flip = fn(x: bool): bool { return !x; };
    )"};
    CHECK(helpers::compile_and_run(fmt::format(R"({}
        pub const main = fn(): i32 {{
            let a: Box(i32) = .{{ .v = 3 }};
            let b: Box(bool) = .{{ .v = false }};
            return a.map(neg) + (if (b.map(flip)) 10 else 0);
        }};
    )",
                                               boxed)) == -3 + 10);
    CHECK(helpers::raised(fmt::format(R"({}
        pub const main = fn(): i32 {{
            let b: Box(bool) = .{{ .v = false }};
            return b.map(neg);
        }};
    )",
                                      boxed),
                          sema::error::UNSATISFIED_BOUND));
}

} // namespace ghoti::tests
