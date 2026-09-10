#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view RESULT_PRELUDE = R"(
const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
impl(T: type, E: type) builtin.Unwrappable for Result(T, E) {
    using Output = T;
    using Residual = E;
    pub const branch := fn(self): builtin.Flow(T, E) {
        return match (self) {
            .ok => |v| builtin.Flow(T, E){ .@"continue" = v },
            .err => |e| builtin.Flow(T, E){ .@"break" = e },
        };
    };
}
impl(T: type, E: type) builtin.Rewrappable for Result(T, E) {
    using From = E;
    pub const fromResidual := fn(r: E): @this() { return .{ .err = r }; };
}
const Option := fn(T: type): type { return union { some: T, none: void }; };
impl(T: type) builtin.Unwrappable for Option(T) {
    using Output = T;
    using Residual = void;
    pub const branch := fn(self): builtin.Flow(T, void) {
        return match (self) {
            .some => |v| builtin.Flow(T, void){ .@"continue" = v },
            .none => builtin.Flow(T, void){ .@"break" = {} },
        };
    };
}
impl(T: type) builtin.Rewrappable for Option(T) {
    using From = void;
    pub const fromResidual := fn(_: void): @this() { return .{ .none = {} }; };
}
)";

} // namespace

TEST_CASE("errdefer executes on `?` error propagation") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const fail := fn(x: i32): R {
            return if (x < 0) R{ .err = 5 }; else R{ .ok = x };
        };
        const run := fn(x: i32, log: ^mut i32): R {
            errdefer *log = 42;
            const v := fail(x)?;
            return R{ .ok = v };
        };
        pub const main := fn(): i32 {
            var x: i32 = -1;
            var log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 42);
}

TEST_CASE("errdefer does not execute on success") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const succeed := fn(x: i32): R {
            return if (x < 0) R{ .err = 5 }; else R{ .ok = x };
        };
        const run := fn(x: i32, log: ^mut i32): R {
            errdefer *log = 42;
            const v := succeed(x)?;
            return R{ .ok = v };
        };
        pub const main := fn(): i32 {
            var x: i32 = 10;
            var log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 0);
}

TEST_CASE("defer and errdefer interleave in LIFO order") {
    SECTION("on error path, both defer and errdefer run in reverse order") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R := Result(i32, i32);
            const fail := fn(x: i32): R {
                return if (x < 0) R{ .err = 9 }; else R{ .ok = x };
            };
            const run := fn(x: i32, log: ^mut i32): R {
                defer *log = *log * 10 + 1;
                errdefer *log = *log * 10 + 2;
                defer *log = *log * 10 + 3;
                const v := fail(x)?;
                return R{ .ok = v };
            };
            pub const main := fn(): i32 {
                var x: i32 = -1;
                var log: i32 = 0;
                _ = run(x, ^mut log);
                return log; // 3, then 2, then 1 -> 321
            };
        )") == 321);
    }

    SECTION("on success path, only plain defers run in reverse order") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R := Result(i32, i32);
            const succeed := fn(x: i32): R {
                return if (x < 0) R{ .err = 9 }; else R{ .ok = x };
            };
            const run := fn(x: i32, log: ^mut i32): R {
                defer *log = *log * 10 + 1;
                errdefer *log = *log * 10 + 2;
                defer *log = *log * 10 + 3;
                const v := succeed(x)?;
                return R{ .ok = v };
            };
            pub const main := fn(): i32 {
                var x: i32 = 9;
                var log: i32 = 0;
                _ = run(x, ^mut log);
                return log; // 3, then 1 -> 31
            };
        )") == 31);
    }
}

TEST_CASE("errdefer by-value capture receives error payload") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const fail := fn(x: i32): R {
            return if (x < 0) R{ .err = 77 }; else R{ .ok = x };
        };
        const run := fn(x: i32, log: ^mut i32): R {
            errdefer |e| *log = e;
            const v := fail(x)?;
            return R{ .ok = v };
        };
        pub const main := fn(): i32 {
            var x: i32 = -1;
            var log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 77);
}

TEST_CASE("errdefer discard capture works") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const fail := fn(x: i32): R {
            return if (x < 0) R{ .err = 1 }; else R{ .ok = x };
        };
        const run := fn(x: i32, log: ^mut i32): R {
            errdefer |_| *log = 99;
            const v := fail(x)?;
            return R{ .ok = v };
        };
        pub const main := fn(): i32 {
            var x: i32 = -1;
            var log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 99);
}

TEST_CASE("errdefer alias captures read error payload") {
    SECTION("reference capture") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R := Result(i32, i32);
            const fail := fn(x: i32): R {
                return if (x < 0) R{ .err = 55 }; else R{ .ok = x };
            };
            const run := fn(x: i32, log: ^mut i32): R {
                errdefer |&e| *log = e;
                const v := fail(x)?;
                return R{ .ok = v };
            };
            pub const main := fn(): i32 {
                var x: i32 = -1;
                var log: i32 = 0;
                _ = run(x, ^mut log);
                return log;
            };
        )") == 55);
    }

    SECTION("pointer capture") {
        CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
            const R := Result(i32, i32);
            const fail := fn(x: i32): R {
                return if (x < 0) R{ .err = 66 }; else R{ .ok = x };
            };
            const run := fn(x: i32, log: ^mut i32): R {
                errdefer |^p| *log = *p;
                const v := fail(x)?;
                return R{ .ok = v };
            };
            pub const main := fn(): i32 {
                var x: i32 = -1;
                var log: i32 = 0;
                _ = run(x, ^mut log);
                return log;
            };
        )") == 66);
    }
}

TEST_CASE("errdefer with Option capturing void") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const O := Option(i32);
        const fail := fn(x: i32): O {
            return if (x < 0) O{ .none = {} }; else O{ .some = x };
        };
        const run := fn(x: i32, log: ^mut i32): O {
            errdefer |e| {
                const check: void = e;
                *log = 42;
            }
            const v := fail(x)?;
            return O{ .some = v };
        };
        pub const main := fn(): i32 {
            var x: i32 = -1;
            var log: i32 = 0;
            _ = run(x, ^mut log);
            return log;
        };
    )") == 42);
}

TEST_CASE("errdefer does not trigger on direct static error return under Definition A") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const direct_err := fn(log: ^mut i32): R {
            errdefer *log = 100;
            return R{ .err = 1 };
        };
        pub const main := fn(): i32 {
            var log: i32 = 0;
            _ = direct_err(^mut log);
            return log; // Remains 0 because static return does not trigger errdefer under Definition A
        };
    )") == 0);
}

} // namespace ghoti::tests
