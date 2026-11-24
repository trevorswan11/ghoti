#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <stdx/option.hh>

#include "compiler/ip/pool.hh"
#include "compiler/module/memory_loader.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

// The same GIR, or the same effects with values computed in another place
[[nodiscard]] auto agrees(const helpers::uir_diff& diff) -> bool {
    return diff.status == helpers::uir_diff_status::MATCH ||
           diff.status == helpers::uir_diff_status::EVAL_ORDER ||
           diff.status == helpers::uir_diff_status::FOLD_PLACEMENT;
}

} // namespace

TEST_CASE("`@TypeOf` of a `let mut` global asks only its type", "[uir]") {
    const auto diff{diff_source(R"(
        let mut a: i16 = undefined;
        let mut b: i64 = undefined;
        const same = @TypeOf(a, b) == i64;
        pub const main = fn(): i32 { return if comptime (same) 1 else 2; };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A runtime body writes and reads the fields and elements of comptime memory", "[uir]") {
    const auto diff{diff_source(R"(
        const P = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            comptime let mut p: P = .{ .x = 1, .y = 2 };
            p.x = 10;
            p.y += 5;
            comptime let mut arr: [2]i32 = .{4, 5};
            arr[0] = 6;
            return p.x + p.y + arr[0] + arr[1];
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A compile-time reference or pointer writes the memory it names", "[uir]") {
    const auto diff{diff_source(R"(
        const S = struct { x: i32, arr: [3]mut i32 };
        const bump = fn(p: &mut i32): void { p += 5; };
        const first = fn(a: &mut [2]mut i32): &mut i32 { return &mut a[0]; };
        const t = fn(): i32 {
            let mut x: i32 = 4;
            let r = &mut x;
            r += 3;
            let p = ^mut x;
            *p = *p + 1;
            bump(&mut x);
            bump(&mut x);
            let mut s: S = .{ .x = 1, .arr = .{1, 2, 3} };
            let e = &mut s.arr[1];
            e = 20;
            let q = ^mut s;
            q.x = 7;
            let mut a = [_]mut i32{1, 2};
            let f = first(&mut a);
            f = 30;
            return x + r + s.x + s.arr[1] + a[0];
        };
        const folded = t();
        pub const main = fn(): i32 { return folded; };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));

    const auto run{helpers::uir_accepted(R"(
        const bump = fn(p: &mut i32): void { p += 5; };
        const t = fn(): i32 {
            let mut x: i32 = 4;
            bump(&mut x);
            bump(&mut x);
            return x;
        };
        const folded = t();
        pub const main = fn(): i32 { return folded; };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("ret i32 14"));
}

TEST_CASE("A match capture of a compile-time aggregate is read at runtime", "[uir]") {
    const auto diff{diff_source(R"(
        const Color = enum { red, green, blue };
        pub const main = fn(): i32 {
            return match comptime (@typeInfo(Color)) {
                .@"enum" => |e| if (e.exhaustive) 1 else 0,
                _ => -1,
            };
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

// #446: a `^fn` field of a constant struct holds the function like an erased `fn` field
TEST_CASE("A function in a constant struct's `^fn` field is called through it", "[uir]") {
    constexpr std::string_view source{R"(
        const dbl = fn(x: i32): i32 { return x * 2; };
        const Hook = struct { f: ^fn(x: i32): i32 };
        const HOOK = Hook{ .f = dbl };
        pub const main = fn(): i32 { return HOOK.f(5); };
    )"};
    const auto                 diff{diff_source(source)};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));

    const auto  run{helpers::uir_accepted(source)};
    const auto& globals{run.out().gir->get_globals()};
    const auto  global{
        std::ranges::find_if(globals, [](const auto g) { return g->name == "HOOK"; })};
    REQUIRE(global != globals.end());
    REQUIRE((*global)->const_init);
    const auto& init{run.pool().value_at(*(*global)->const_init).data};
    REQUIRE(init.is<ip::aggregate_value>());
    const auto field{init.as<ip::aggregate_value>().elements.front()};
    REQUIRE(field);
    const auto& f{run.pool().value_at(*field).data};
    REQUIRE(f.is<std::string>());
    CHECK(f.as<std::string>() == "dbl");
}

TEST_CASE("A method on comptime memory runs at compile time and writes it", "[uir]") {
    const auto diff{diff_source(R"(
        const Counter = struct {
            n: i32,
            pub const bump = fn(&mut self, by: i32): i32 {
                self.n += by;
                return self.n;
            };
        };
        pub const main = fn(): i32 {
            comptime let mut c: Counter = .{ .n = 1 };
            comptime let mut last: i32 = 0;
            last = c.bump(4);
            last = c.bump(last);
            return last + c.n;
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("An `if comptime` reads a compile-time-only value's elements", "[uir]") {
    const auto diff{diff_source(R"(
        const U = union(enum) { some: i32, none: void };
        const eql = fn(a: []u8, b: []u8): bool {
            if (a.len != b.len) return false;
            for (a, b) |x, y| { if (x != y) return false; }
            return true;
        };
        const shape = fn(comptime T: type): i32 {
            const fields = @typeInfo(T).union.fields;
            if comptime (eql(fields[0].name, "some")) { return 1; }
            return 0;
        };
        pub const main = fn(): i32 { return shape(U); };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A parameterized impl names its target's ctor through an alias", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const Named = interface { const name = fn(&self): i32; };
        const Box = fn(comptime T: type): type { return struct { v: T }; };
        const Alias = Box;
        impl(T: type) Named for Alias(T) {
            const name = fn(&self): i32 { return 7; };
        }
        pub const main = fn(): i32 {
            return if comptime (@implements(Box(i32), Named)) 1 else 2;
        };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("ret i32 1"));
}

TEST_CASE("A compile-time slice or element pointer writes the array it views", "[uir]") {
    const auto diff{diff_source(R"(
        const fill = fn(s: []mut i32): void { s[0] = 40; };
        const get = fn(s: []mut? i32, i: usize): &mut? i32 { return &mut? s[i]; };
        const t = fn(): i32 {
            let a = [_]mut i32{1, 2, 3};
            fill(a);
            let s: []mut i32 = a[1..];
            s[0] = 50;
            let inner: []mut i32 = s[1..];
            inner[0] = 60;
            let e = get(a, 2);
            e += 1;
            let mut b = [3]mut i32{ 1, 2, 3 };
            let p: ^mut i32 = b.ptr;
            p[2] = 4;
            let q = ^mut b[1];
            q[1] += 1;
            return a[0] + a[1] + a[2] + @intCast(i32, s.len) + b[2] + p[1] * 10;
        };
        const folded = t();
        pub const main = fn(): i32 { return folded + t(); };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));

    const auto run{helpers::uir_accepted(R"(
        const fill = fn(s: []mut i32): void { s[0] = 40; };
        const t = fn(): i32 { let a = [_]mut i32{1, 2, 3}; fill(a); return a[0]; };
        const folded = t();
        pub const main = fn(): i32 { return folded; };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("ret i32 40"));
}

TEST_CASE("Comptime memory holds union fields and vector lanes", "[uir]") {
    const auto diff{diff_source(R"(
        const U = union(enum) { i: i32, f: f64 };
        const W = struct { u: U, xs: [3]i32 };
        const build = fn(): i32 {
            comptime let mut w: W = .{ .u = .{ .i = 1 }, .xs = .{ 1, 2, 3 } };
            w.u.i += 4;
            w.u.f = 2.5;
            return w.xs[2] + @as(i32, @intFromFloat(w.u.f * 2.0));
        };
        const iota = fn(comptime V: type): V {
            let mut v: V = undefined;
            for (0..@typeInfo(V).vector.len) |i| { v[i] = @intCast(i); }
            return v;
        };
        const AT = build();
        pub const main = fn(): i32 {
            comptime {
                const v = iota(@Vector(4, u8));
                @assert(v[3] == 3);
            }
            return AT;
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A compile-time dereference reads a known slice or undefined pointer", "[uir]") {
    const auto diff{diff_source(R"(
        const p: ^i32 = undefined;
        const val_p = *p;
        const pick = fn(): [2]u8 {
            const s = "hello";
            return *s[1..3];
        };
        const x = pick();
        pub const main = fn(): i32 {
            let r: i32 = x[0] + x[1];
            return r - 200;
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
