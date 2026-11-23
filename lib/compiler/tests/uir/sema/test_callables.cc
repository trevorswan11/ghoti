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

TEST_CASE("A function item is an erased `fn`, and a `let` naming it calls it directly", "[uir]") {
    const auto diff{diff_source(R"(
        const apply = fn(f: fn(x: i32): i32, v: i32): i32 { return f(v); };
        const inc = fn(x: i32): i32 { return x + 1; };
        pub const main = fn(): i32 {
            let g: fn(x: i32): i32 = inc;
            let h = inc;
            let mut m: fn(x: i32): i32 = inc;
            m = inc;
            return apply(g, 1) + g(2) + h(3) + apply(h, 4) + m(5);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A closure stores each capture by value, borrowed or by `&mut`", "[uir]") {
    const auto diff{diff_source(R"(
        const apply = fn(f: fn(x: i32): i32, v: i32): i32 { return f(v); };
        pub const main = fn(): i32 {
            let mut base: i32 = 3;
            let add = fn(x: i32): i32 { return x + base; };
            base = 4;
            let mut total: i32 = 0;
            let bump = fn(): void { total = total + 1; };
            bump();
            bump();
            let s = [3]i32{ 1, 2, 3 };
            let sum = fn(): i32 { return s[0] + s[2]; };
            return apply(add, 1) + add(2) + total + sum();
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A local function captures only `const`s and is called by its own name", "[uir]") {
    const auto diff{diff_source(R"(
        const apply = fn(f: fn(x: i32): i32, v: i32): i32 { return f(v); };
        pub const main = fn(): i32 {
            const k: i32 = 5;
            let twice = fn(x: i32): i32 { return x * 2 + k; };
            const setit = fn(c: &mut i32): void { *c = 9; };
            let mut v: i32 = 0;
            setit(&mut v);
            return twice(3) + apply(twice, 4) + fn(x: i32): i32 { return x * 3; }(v);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A capturing generic local function takes its environment and recurses by name",
          "[uir]") {
    const auto diff{diff_source(R"(
        pub const main = fn(): i32 {
            let off: i32 = 1;
            let f = fn(x: auto): i32 {
                if (x == 0) { return off; }
                return f(x - 1) + off;
            };
            let twice = fn(x: auto): @TypeOf(x) { return x * 2; };
            return f(@as(i32, 3)) + twice(@as(i32, 4)) + f(@as(u8, 2));
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A local generic of each generic instance is its own", "[uir]") {
    const auto diff{diff_source(R"(
        const outer = fn(comptime T: type, base: T): T {
            let f = fn(x: auto): T { return base + @as(T, @intCast(x)); };
            return f(@as(u8, 1)) + f(@as(i64, 2));
        };
        pub const main = fn(): i32 {
            return outer(i32, 10) + @as(i32, @intCast(outer(i64, 20)));
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("`@fnCtx()` is the closure itself and `.thunk` its body", "[uir]") {
    const auto diff{diff_source(R"(
        pub const main = fn(): i32 {
            let mut calls: i32 = 0;
            let fact = fn(n: i32): i32 {
                calls = calls + 1;
                if (n <= 1) { return 1; }
                return n * @fnCtx()(n - 1);
            };
            let mut offset: i32 = 7;
            let add = fn(x: i32): i32 { return x + offset; };
            let thunk = add.thunk;
            return fact(5) + calls + thunk(&mut add, 5);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A `^fn` is a nullable erased callable and `&f` is the function itself", "[uir]") {
    const auto diff{diff_source(R"(
        const apply = fn(func: ^fn(x: i32): i32, val: i32): i32 { return func(val); };
        const double_it = fn(x: i32): i32 { return x * 2; };
        const target = fn(x: i32): i32 { return x + 1; };
        const BinOp = fn(lhs: i32, rhs: i32): i32;
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        const fold = fn(op: BinOp, x: i32, y: i32): i32 { return op(x, y); };
        pub const main = fn(): i32 {
            let mut fptr: ^fn(n: i32): i32 = target;
            let r: &fn(x: i32): i32 = &double_it;
            return apply(double_it, 5) + fptr(42) + apply(r, 1) + fold(add, 1, 2);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A function passed to a `comptime` parameter is a copy of it the instance calls",
          "[uir]") {
    const auto diff{diff_source(R"(
        const inc = fn(x: i32): i32 { return x + 1; };
        const apply = fn(comptime f: fn(n: i32): i32, v: i32): i32 { return f(f(v)); };
        pub const main = fn(): i32 {
            let step: i32 = 3;
            return apply(inc, 40) + apply(fn(x: i32): i32 { return x + step; }, 36);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A generic function is a value of its own type", "[uir]") {
    const auto diff{diff_source(R"(
        const apply = fn(g: auto, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };
        const doubled = apply(twice, 21);
        pub const main = fn(): i32 { return apply(twice, 1) + doubled; };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A closure at compile time writes the memory it borrows", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const sum_with = fn(n: i32, bias: i32): i32 {
            let mut s: i32 = 0;
            let add = fn(x: i32): void { s += x + bias; };
            for (0..n) |i| { add(@intCast(i)); }
            return s;
        };
        const total = sum_with(3, 1);
        pub const main = fn(): i32 { return total; };
    )")};
    CHECK_THAT(run.gir(), Catch::Matchers::ContainsSubstring("ret i32 6"));
}

TEST_CASE("`@call` modifiers mark the call or run it at compile time", "[uir]") {
    const auto diff{diff_source(R"(
        const add = fn(a: i32, b: i32): i32 { return a + b; };
        const square = fn(x: i32): i32 { return x * x; };
        const count = fn(n: i32, acc: i32): i32 {
            if (n == 0) { return acc; }
            return @call(.always_tail, count, n - 1, acc + 1);
        };
        pub const main = fn(): i32 {
            let mut out: i32 = @call(.auto, add, 1, 2);
            out += @call(.never_inline, add, 3, 4);
            const folded = @call(.compile_time, square, 4);
            return out + folded + count(10, 0);
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

TEST_CASE("A returned failure runs the errdefers in scope with its residual", "[uir]") {
    const auto diff{diff_source(R"(
        const Result = fn(comptime T: type, comptime E: type): type {
            return union(enum) { ok: T, err: E };
        };
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
        const R = Result(i32, u8);
        const work = fn(mode: i32, log: ^mut i32): R {
            defer *log = *log * 10 + 1;
            errdefer |e| *log = *log * 10 + @as(i32, e);
            if (mode == 0) { return R{ .err = 7 }; }
            let r: R = if (mode == 2) .{ .err = 3 } else .{ .ok = 9 };
            return r;
        };
        pub const main = fn(): i32 {
            let mut log: i32 = 0;
            _ = work(2, ^mut log);
            return log;
        };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));
}

// #446: a function stored in a compile-time struct is called through the global holding it
TEST_CASE("A function in a constant struct is called through its field", "[uir]") {
    const auto diff{diff_source(R"(
        const inc = fn(x: i32): i32 { return x + 1; };
        const Config = struct { poly: fn(x: i32): i32, scale: i32 };
        const C = Config{ .poly = inc, .scale = 2 };
        pub const main = fn(): i32 { return C.poly(3) * C.scale; };
    )")};
    INFO(diff.detail);
    INFO(helpers::uir_diff_status_name(diff.status));
    CHECK(agrees(diff));

    // The global holds the function itself, which codegen pairs with its trampoline
    const auto  run{helpers::uir_accepted(R"(
        const inc = fn(x: i32): i32 { return x + 1; };
        const Config = struct { poly: fn(x: i32): i32, scale: i32 };
        const C = Config{ .poly = inc, .scale = 2 };
        pub const main = fn(): i32 { return C.poly(3) * C.scale; };
    )")};
    const auto& globals{run.out().gir->get_globals()};
    const auto  global{std::ranges::find_if(globals, [](const auto g) { return g->name == "C"; })};
    REQUIRE(global != globals.end());
    REQUIRE((*global)->const_init);
    const auto& init{run.pool().value_at(*(*global)->const_init).data};
    REQUIRE(init.is<ip::aggregate_value>());
    const auto field{init.as<ip::aggregate_value>().elements.front()};
    REQUIRE(field);
    const auto& poly{run.pool().value_at(*field).data};
    REQUIRE(poly.is<std::string>());
    CHECK(poly.as<std::string>() == "inc");
}

} // namespace ghoti::tests
