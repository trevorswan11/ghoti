#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view PRELUDE{R"(
const Result = fn(T: type, E: type): type { return union { ok: T, err: E }; };
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
const Option = fn(T: type): type { return union { some: T, none: void }; };
impl(T: type) builtin.Unwrappable for Option(T) {
    const Output = T;
    const Residual = void;
    pub const branch = fn(&mut? self): builtin.Flow(&mut? T, void) {
        return match (self) {
            .some => |&mut? v| .{ .@"continue" = v },
            .none => .{ .@"break" = {} },
        };
    };
}
const O = Option(i32);
const R = Result(i32, u8);
const mk = fn(n: i32): O { return if (n > 0) O{ .some = n } else O{ .none = {} }; };
const half = fn(n: i32): R { return if (n % 2 == 0) R{ .ok = n / 2 } else R{ .err = 7 }; };
const Counter = struct {
    n: i32,
    limit: i32,
    pub const next = fn(&mut self): O {
        if (self.n >= self.limit) { return O{ .none = {} }; }
        self.n += 1;
        return O{ .some = self.n };
    };
};
)"};

// Runs `t()` both at runtime and folded through `comptime t()`; each must give `expected`
auto check_both(std::string_view defs, u32 expected) -> void {
    const auto runtime{
        fmt::format("{}{}\npub const main = fn(): i32 {{ return t(); }};", PRELUDE, defs)};
    const auto folded{
        fmt::format("{}{}\npub const main = fn(): i32 {{ return comptime t(); }};", PRELUDE, defs)};
    CHECK(helpers::compile_and_run(runtime) == expected);
    CHECK(helpers::compile_and_run(folded) == expected);
}

auto expect_error(std::string_view source, sema::error code) -> void {
    const auto [ctx, idx]{helpers::expect_compile_error(fmt::format("{}{}", PRELUDE, source))};
    const auto& diags{ctx->root_mod.diagnostics.as<sema::diagnostics>()};
    CHECK(std::ranges::any_of(diags, [&](const auto& diag) { return diag.get_error() == code; }));
}

} // namespace

TEST_CASE("`if (x) |v|` takes the payload or falls to `else`") {
    check_both(R"(
        const t = fn(): i32 {
            let mut total: i32 = 0;
            if (mk(4)) |v| { total += v; } else { total += 100; }
            if (mk(0)) |v| { total += v; } else { total += 10; }
            if (mk(2)) |_| { total += 100; }
            return total;
        };
    )",
               114);
}

TEST_CASE("`if` is an expression whose `else |e|` captures the residual") {
    check_both(R"(
        const t = fn(): i32 {
            let a = if (half(8)) |v| v else |e| @as(i32, e);
            let b = if (half(3)) |v| v else |e| @as(i32, e) + 10;
            return a * 10 + b;
        };
    )",
               57);
}

TEST_CASE("`|&mut v|` and `|^mut p|` captures write into the operand") {
    check_both(R"(
        const t = fn(): i32 {
            let mut o = mk(5);
            if (o) |&mut v| { v += 30; }
            let mut r = half(4);
            if (r) |^mut p| { *p = 9; } else |_| {}
            let s = mk(6);
            let by_ref = if (s) |&v| v + 1 else 0;
            return match (o) { .some => |v| v, .none => 0 } +
                   match (r) { .ok => |v| v, .err => |e| @as(i32, e) } + by_ref;
        };
    )",
               51);
}

TEST_CASE("`while (x) |v|` iterates until `branch` breaks") {
    check_both(R"(
        const t = fn(): i32 {
            let mut c: Counter = .{ .n = 0, .limit = 4 };
            let mut sum: i32 = 0;
            while (c.next()) |x| { sum += x; }
            return sum;
        };
    )",
               10);
}

TEST_CASE("`while (x) |v|` with a continuation, `break`, and `else`") {
    check_both(R"(
        const t = fn(): i32 {
            let mut c: Counter = .{ .n = 0, .limit = 4 };
            let mut sum: i32 = 0;
            while (c.next()) |x| : (sum += 100) { if (x == 3) { break; } sum += x; } else { sum += 1000; }
            let mut d: Counter = .{ .n = 0, .limit = 2 };
            let mut s: i32 = 0;
            while (d.next()) |x| { s += x; } else { s += 50; }
            return (sum - 200) + s;
        };
    )",
               56);
}

TEST_CASE("`while (x) |v| ... else |e|` captures the residual that ended it") {
    check_both(R"(
        const Countdown = struct {
            n: i32,
            pub const step = fn(&mut self): R {
                if (self.n == 0) { return R{ .err = 42 }; }
                self.n -= 1;
                return R{ .ok = self.n };
            };
        };
        const t = fn(): i32 {
            let mut c: Countdown = .{ .n = 3 };
            let mut seen: i32 = 0;
            while (c.step()) |v| { seen += v; } else |e| { seen += @as(i32, e); }
            return seen;
        };
    )",
               45);
}

TEST_CASE("captures fold in constants, `if comptime`, and `while comptime`") {
    CHECK(helpers::compile_and_run(fmt::format("{}{}", PRELUDE, R"(
        const OPT: O = O{ .some = 12 };
        const X = if (OPT) |v| v + 1 else 0;
        const Y = if (half(5)) |v| v else |e| @as(i32, e) * 2;
        pub const main = fn(): i32 {
            let mut total: i32 = X + Y;
            if comptime (mk(3)) |v| { total += v; } else { total += 100; }
            if comptime (half(1)) |v| { total += v; } else |e| { total += @as(i32, e); }
            comptime let mut c: Counter = .{ .n = 0, .limit = 3 };
            while comptime (c.next()) |x| { total += x * 10; }
            return total;
        };
    )")) == 13 + 14 + 3 + 7 + 60);
}

TEST_CASE("misused `if` / `while` captures are errors") {
    expect_error("const f = fn(o: O): void { if (o) { } };", sema::error::ILLEGAL_UNWRAP_CAPTURE);
    expect_error("const f = fn(o: O): void { while (o) { break; } };",
                 sema::error::ILLEGAL_UNWRAP_CAPTURE);
    expect_error("const f = fn(b: bool): void { if (b) |v| { _ = v; } };",
                 sema::error::ILLEGAL_UNWRAP_CAPTURE);
    expect_error("const f = fn(o: O): void { if (o) |v| { _ = v; } else |e| { _ = e; } };",
                 sema::error::ILLEGAL_UNWRAP_CAPTURE);
    expect_error("const f = fn(r: R): void { if (r) |v| { _ = v; } else |&e| { _ = e; } };",
                 sema::error::ILLEGAL_UNWRAP_CAPTURE);
    expect_error("const f = fn(): void { if comptime (mk(1)) |&v| { _ = v; } };",
                 sema::error::ILLEGAL_UNWRAP_CAPTURE);
    // A mutable capture needs a mutable place
    expect_error("const f = fn(o: O): void { if (o) |&mut v| { v = 1; } };",
                 sema::error::ASSIGNMENT_TO_CONST);
    expect_error("const f = fn(): void { if (mk(1)) |&mut v| { v = 1; } };",
                 sema::error::ASSIGNMENT_TO_CONST);
    // A capture is an immutable binding
    expect_error(R"(
        const f = fn(o: O): i32 { if (o) |v| { v = 1; return v; } return 0; };
        pub const main = fn(): i32 { return f(mk(1)); };
    )",
                 sema::error::ASSIGNMENT_TO_CONST);
}

TEST_CASE("a `while` continuation sees the payload capture") {
    check_both(R"(
        const t = fn(): i32 {
            let mut c: Counter = .{ .n = 0, .limit = 3 };
            let mut s: i32 = 0;
            while (c.next()) |x| : (s += x) { if (x == 2) { continue; } }
            return s;
        };
    )",
               6);
}

} // namespace ghoti::tests
