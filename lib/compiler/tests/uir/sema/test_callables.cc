#include <algorithm>
#include <string>
#include <string_view>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <fmt/format.h>
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

TEST_CASE("A call leaving defaulted arguments off goes to the declaration's wrapper", "[uir]") {
    const auto diff{diff_source(R"(
        const add = fn(a: i32, b: i32 = 2, c: i32 = a + 1): i32 { return a + b + c; };
        const pick = fn(comptime T: type, x: T, y: T = 7): T { return if (x > y) x; else y; };
        pub const f = fn(): i32 { return add(1) + add(1, 5) + pick(i32, 3); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A method's defaults reach calls through a value and through its type", "[uir]") {
    const auto diff{diff_source(R"(
        const Counter = struct {
            value: i32,
            pub const bump = fn(&mut self, by: i32 = 1): void { self.value += by; };
            pub const get = fn(&self, offset: i32 = 0): i32 { return self.value + offset; };
        };
        pub const f = fn(): i32 {
            let mut c: Counter = .{ .value = 5 };
            c.bump();
            Counter.bump(&mut c);
            return c.get() + Counter.get(&c);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`^r` of a reference is the address the reference holds", "[uir]") {
    const auto diff{diff_source(R"(
        const Holder = struct { r: &i32 };
        pub const main = fn(): i32 {
            let mut x: i32 = 40;
            let r: &i32 = &x;
            let p: ^i32 = ^r;
            let h = Holder{ .r = &x };
            let q: ^i32 = ^(h.r);
            let rm: &mut i32 = &mut x;
            let pm = ^mut rm;
            *pm = 1;
            return *p + *q;
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("Generics, closures and references to them become erased `fn`s", "[uir]") {
    const auto diff{diff_source(R"(
        const Op = struct { f: fn(n: i32): i32 };
        const apply = fn(g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        const apply_ptr = fn(g: ^fn(n: i32): i32, v: i32): i32 { return g(v); };
        const fixed = fn(comptime g: fn(n: i32): i32, v: i32): i32 { return g(v); };
        const twice = fn(x: auto): @TypeOf(x) { return x * 2; };
        pub const main = fn(): i32 {
            let off: i32 = 2;
            let g = fn(x: auto): i32 { return @as(i32, @intCast(x)) + off; };
            let c = fn(x: i32): i32 { return x + off; };
            let mut h: fn(n: i32): i32 = g;
            h = twice;
            let p: ^fn(n: u8): i32 = g;
            let op: Op = .{ .f = g };
            return h(2) + p(@as(u8, 3)) + op.f(4) + apply(&c, 1) + apply(&g, 2) +
                   apply_ptr(twice, 8) + fixed(twice, 21);
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("`.name` as a function names the member of the type it returns", "[uir]") {
    const auto diff{diff_source(R"(
        const T = struct {
            x: i32,
            const init = fn(): T { return T{ .x = 42 }; };
        };
        pub const main = fn(): i32 {
            let f: fn(): T = .init;
            return f().x;
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A null `^fn` is zeroed, and `*p` calls what it holds", "[uir]") {
    const auto diff{diff_source(R"(
        const inc = fn(n: i32): i32 { return n + 1; };
        pub const main = fn(): i32 {
            let mut p: ^fn(n: i32): i32 = nullptr;
            let mut score: i32 = 0;
            if (p == nullptr) { score += 1; }
            p = inc;
            let q: ^fn(n: i32): i32 = ^inc;
            return score + p(4) + (*p)(5) + (*q)(6);
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A C-variadic function reads its extra arguments through `va_list` storage", "[uir]") {
    const auto diff{diff_source(R"(
        const sum = fn(n: i32, ...): i32 {
            let mut storage: [4]mut usize = undefined;
            let ap = @ptrCast(^mut opaque, ^mut storage);
            @cVaStart(ap);
            let mut total: i32 = 0;
            let mut i: i32 = 0;
            while (i < n) : (i += 1) { total += @cVaArg(ap, i32); }
            @cVaEnd(ap);
            return total;
        };
        pub const main = fn(): i32 { return sum(3, 1, 2, 4); };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));

    const auto outside{helpers::uir_rejected(R"(
        pub const f = fn(n: i32): i32 {
            let mut storage: [4]mut usize = undefined;
            @cVaStart(@ptrCast(^mut opaque, ^mut storage));
            return n;
        };
    )")};
    CHECK_THAT(fmt::format("{}", outside.out().diagnostics),
               Catch::Matchers::ContainsSubstring("can only be used inside a C-variadic"));
}

TEST_CASE("A function literal a top-level statement discards is still checked", "[uir]") {
    const auto diff{diff_source(R"(
        const ap = fn(g: auto, v: i32): i32 { return g(v); };
        _ = fn(): i32 {
            let k: i32 = 5;
            return ap(fn(n: i32): i32 { return n + k; }, 1);
        };
        pub const main = fn(): i32 { return 3; };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));

    const auto bad{helpers::uir_rejected("_ = fn(): i32 { return true; };")};
    CHECK_FALSE(bad.out().diagnostics.empty());
}

TEST_CASE("A `move fn` run at compile time keeps a copy it writes", "[uir]") {
    const auto run{helpers::uir_accepted(R"(
        const run = fn(): i32 {
            let mut c: i32 = 0;
            let bump = move fn(x: i32): i32 { c += x; return c; };
            _ = bump(1);
            return bump(2) * 10 + c;
        };
        const moved = run();
    )")};
    CHECK(run.int_value("moved") == 30);
}

TEST_CASE("Local functions and interface methods take their defaults", "[uir]") {
    const auto diff{diff_source(R"(
        const Shape = interface {
            pub const scaled = fn(&self, by: i32 = 2, extra: i32 = 0): i32;
        };
        const Sq = struct { s: i32 };
        impl Shape for Sq {
            pub const scaled = fn(&self, by: i32, extra: i32): i32 { return self.s * by + extra; };
        }
        const through_dyn = fn(s: &dyn Shape): i32 { return s.scaled(); };
        const through_bound = fn(s: &impl Shape): i32 { return s.scaled(3); };
        pub const main = fn(): i32 {
            const twice = fn(x: i32, times: i32 = 2): i32 { return x * times; };
            let sq = Sq{ .s = 5 };
            return sq.scaled() + through_dyn(&sq) + through_bound(&sq) + twice(4);
        };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A closure copies the pack it captures, or only its length", "[uir]") {
    const auto diff{diff_source(R"(
        const outer = fn(rest...): i32 {
            let len = fn(): i32 { return @intCast(rest.len); };
            let first = fn(): i32 { return @as(i32, @intCast(rest[0])); };
            return len() + first();
        };
        pub const main = fn(): i32 { return outer(5, 6, 7) + outer(@as(i64, 9)); };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

TEST_CASE("A parameter's type may ask an earlier parameter's type", "[uir]") {
    const auto diff{diff_source(R"(
        const a = fn(b: i32, c: ^@TypeOf(b)): i32 { return b + *c; };
        pub const main = fn(): i32 { let x: i32 = 1; return a(2, ^x); };
    )")};
    INFO(diff.detail);
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
