#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

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
const Option = fn(comptime T: type): type { return union(enum) { some: T, none: void }; };
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
impl(T: type) builtin.Rewrappable for Option(T) {
    const From = void;
    pub const from_residual = fn(_: void): @This() { return .{ .none = {} }; };
}
)";

} // namespace

TEST_CASE("errdefer executes on `?` error propagation") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const fail = fn(x: i32): R {
            return if (x < 0) R{ .err = 5 }; else R{ .ok = x };
        };
        const run = fn(x: i32, log: ^mut i32): R {
            errdefer *log = 42;
            let v = fail(x)?;
            return R{ .ok = v };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = -1;
            let mut log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 42);
}

TEST_CASE("errdefer does not execute on success") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const succeed = fn(x: i32): R {
            return if (x < 0) R{ .err = 5 }; else R{ .ok = x };
        };
        const run = fn(x: i32, log: ^mut i32): R {
            errdefer *log = 42;
            let v = succeed(x)?;
            return R{ .ok = v };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = 10;
            let mut log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 0);
}

TEST_CASE("defer and errdefer interleave in LIFO order") {
    SECTION("on error path, both defer and errdefer run in reverse order") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R = Result(i32, i32);
            const fail = fn(x: i32): R {
                return if (x < 0) R{ .err = 9 }; else R{ .ok = x };
            };
            const run = fn(x: i32, log: ^mut i32): R {
                defer *log = *log * 10 + 1;
                errdefer *log = *log * 10 + 2;
                defer *log = *log * 10 + 3;
                let v = fail(x)?;
                return R{ .ok = v };
            };
            pub const main = fn(): i32 {
                let mut x: i32 = -1;
                let mut log: i32 = 0;
                _ = run(x, ^mut log);
                return log - 300; // 3, then 2, then 1 -> 321
            };
        )") == 21);
    }

    SECTION("on success path, only plain defers run in reverse order") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R = Result(i32, i32);
            const succeed = fn(x: i32): R {
                return if (x < 0) R{ .err = 9 }; else R{ .ok = x };
            };
            const run = fn(x: i32, log: ^mut i32): R {
                defer *log = *log * 10 + 1;
                errdefer *log = *log * 10 + 2;
                defer *log = *log * 10 + 3;
                let v = succeed(x)?;
                return R{ .ok = v };
            };
            pub const main = fn(): i32 {
                let mut x: i32 = 9;
                let mut log: i32 = 0;
                _ = run(x, ^mut log);
                return log; // 3, then 1 -> 31
            };
        )") == 31);
    }
}

TEST_CASE("errdefer by-value capture receives error payload") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const fail = fn(x: i32): R {
            return if (x < 0) R{ .err = 77 }; else R{ .ok = x };
        };
        const run = fn(x: i32, log: ^mut i32): R {
            errdefer |e| *log = e;
            let v = fail(x)?;
            return R{ .ok = v };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = -1;
            let mut log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 77);
}

TEST_CASE("errdefer discard capture works") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const fail = fn(x: i32): R {
            return if (x < 0) R{ .err = 1 }; else R{ .ok = x };
        };
        const run = fn(x: i32, log: ^mut i32): R {
            errdefer |_| *log = 99;
            let v = fail(x)?;
            return R{ .ok = v };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = -1;
            let mut log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 99);
}

TEST_CASE("errdefer alias captures read error payload") {
    SECTION("reference capture") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R = Result(i32, i32);
            const fail = fn(x: i32): R {
                return if (x < 0) R{ .err = 55 }; else R{ .ok = x };
            };
            const run = fn(x: i32, log: ^mut i32): R {
                errdefer |&e| *log = e;
                let v = fail(x)?;
                return R{ .ok = v };
            };
            pub const main = fn(): i32 {
                let mut x: i32 = -1;
                let mut log: i32 = 0;
                _ = run(x, ^mut log);
                return log;
            };
        )") == 55);
    }

    SECTION("pointer capture") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R = Result(i32, i32);
            const fail = fn(x: i32): R {
                return if (x < 0) R{ .err = 66 }; else R{ .ok = x };
            };
            const run = fn(x: i32, log: ^mut i32): R {
                errdefer |^p| *log = *p;
                let v = fail(x)?;
                return R{ .ok = v };
            };
            pub const main = fn(): i32 {
                let mut x: i32 = -1;
                let mut log: i32 = 0;
                _ = run(x, ^mut log);
                return log;
            };
        )") == 66);
    }
}

TEST_CASE("errdefer with Option capturing void") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const O = Option(i32);
        const fail = fn(x: i32): O {
            return if (x < 0) O{ .none = {} }; else O{ .some = x };
        };
        const run = fn(x: i32, log: ^mut i32): O {
            errdefer |e| {
                let check: void = e;
                *log = 42;
            }
            let v = fail(x)?;
            return O{ .some = v };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = -1;
            let mut log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 42);
}

TEST_CASE("errdefer runs when a function returns a failure explicitly") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, u8);
        const work = fn(mode: i32, log: ^mut i32): R {
            defer *log = *log * 10 + 1;
            errdefer |e| *log = *log * 10 + @as(i32, e);
            if (mode == 0) { return R{ .err = 7 }; }
            if (mode == 1) { return R{ .ok = 5 }; }
            // Known only at runtime, so `branch` decides
            let r: R = if (mode == 2) .{ .err = 3 } else .{ .ok = 9 };
            return r;
        };
        const run = fn(mode: i32): i32 {
            let mut log: i32 = 0;
            _ = work(mode, ^mut log);
            return log;
        };
        pub const main = fn(): i32 {
            let mut m: i32 = 0;
            m += 0;
            if (run(m) != 71 or run(m + 1) != 1 or run(m + 2) != 31 or run(m + 3) != 1) {
                return 1;
            }
            return 0;
        };
    )") == 0);
}

TEST_CASE("errdefer runs on a returned failure at compile time as at runtime") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, u8);
        const work = fn(mode: i32, log: &mut i32): R {
            defer log = log * 10 + 1;
            errdefer |e| log = log * 10 + @as(i32, e);
            if (mode == 0) { return R{ .err = 7 }; }
            let r: R = if (mode == 2) .{ .err = 3 } else .{ .ok = 9 };
            return r;
        };
        const run = fn(mode: i32): i32 {
            let mut log: i32 = 0;
            _ = work(mode, &mut log);
            return log;
        };
        const folded = [_]i32{ run(0), run(1), run(2) };
        pub const main = fn(): i32 {
            let mut m: i32 = 0;
            m += 0;
            if (folded[0] != run(m) or folded[1] != run(m + 1) or folded[2] != run(m + 2)) {
                return 1;
            }
            return folded[0] + folded[2];
        };
    )") == 71 + 31);
}

TEST_CASE("A returned failure runs only the errdefers still in scope") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, u8);
        const nested = fn(mode: i32, log: ^mut i32): R {
            errdefer *log += 1;
            {
                errdefer *log += 10;
                if (mode == 0) { return R{ .err = 1 }; }
            }
            if (mode == 1) { return R{ .err = 2 }; }
            return R{ .ok = 0 };
        };
        const opt = fn(mode: i32, log: ^mut i32): Option(i32) {
            errdefer *log += 100;
            if (mode == 0) { return .{ .none = {} }; }
            return .{ .some = mode };
        };
        pub const main = fn(): i32 {
            let mut m: i32 = 0;
            m += 0;
            let mut a: i32 = 0;
            let mut b: i32 = 0;
            let mut c: i32 = 0;
            let mut d: i32 = 0;
            let mut e: i32 = 0;
            _ = nested(m, ^mut a);
            _ = nested(m + 1, ^mut b);
            _ = nested(m + 2, ^mut c);
            _ = opt(m, ^mut d);
            _ = opt(m + 1, ^mut e);
            return if (a == 11 and b == 1 and c == 0 and d == 100 and e == 0) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("A return inside a function literal runs none of the enclosing function's defers") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, u8);
        const apply = fn(f: impl Fn(x: i32): i32, v: i32): i32 {
            return f(v);
        };
        const work = fn(log: ^mut i32): R {
            defer *log += 1;
            errdefer *log += 100;
            const doubled = apply(fn(x: i32): i32 {
                if (x > 100) { return 0; }
                return x * 2;
            }, 20);
            if (doubled != 40) { return R{ .err = 1 }; }
            return R{ .ok = doubled };
        };
        pub const main = fn(): i32 {
            let mut log: i32 = 0;
            const r = work(^mut log);
            return if (r == .ok and log == 1) 0 else 1;
        };
    )") == 0);
}

} // namespace ghoti::tests
