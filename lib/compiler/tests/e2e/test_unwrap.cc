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
    pub const isBreak := fn(&self): bool { return match (self) { .ok => false, .err => true }; };
    pub const intoOutput := fn(self): T { return match (self) { .ok => |v| v, .err => @trap() }; };
    pub const intoResidual := fn(self): E { return match (self) { .err => |e| e, .ok => @trap() }; };
}
impl(T: type, E: type) builtin.Rewrappable for Result(T, E) {
    using From = E;
    pub const fromResidual := fn(r: E): @this() { return .{ .err = r }; };
}
const Option := fn(T: type): type { return union { some: T, none: void }; };
impl(T: type) builtin.Unwrappable for Option(T) {
    using Output = T;
    using Residual = void;
    pub const isBreak := fn(&self): bool { return match (self) { .some => false, .none => true }; };
    pub const intoOutput := fn(self): T { return match (self) { .some => |v| v, .none => @trap() }; };
    pub const intoResidual := fn(self): void { return {}; };
}
impl(T: type) builtin.Rewrappable for Option(T) {
    using From = void;
    pub const fromResidual := fn(_: void): @this() { return .{ .none = {} }; };
}
)";

} // namespace

TEST_CASE("`?` yields the ok payload and lets execution continue") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const parse := fn(x: i32): R {
            return if (x < 0) R{ .err = 99 }; else R{ .ok = x + 1 };
        };
        const doubled := fn(x: i32): R {
            const v := parse(x)?;
            return R{ .ok = v * 2 };
        };
        pub const main := fn(): i32 {
            var x: i32 = 20;
            return match (doubled(x)) {
                .ok => |v| v,
                .err => |e| e,
            };
        };
    )") == 42);
}

TEST_CASE("`?` on the err variant propagates out of the enclosing function") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const parse := fn(x: i32): R {
            return if (x < 0) R{ .err = 99 }; else R{ .ok = x + 1 };
        };
        const doubled := fn(x: i32): R {
            const v := parse(x)?;
            return R{ .ok = v * 2 };
        };
        pub const main := fn(): i32 {
            var x: i32 = -5;
            return match (doubled(x)) {
                .ok => |v| v,
                .err => |e| e,
            };
        };
    )") == 99);
}

TEST_CASE("`?` runs enclosing scope defers on the propagation path") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const inner := fn(seed: i32, log: ^mut i32): R {
            defer *log = *log + 10;
            const v := (if (seed < 0) R{ .err = 5 }; else R{ .ok = seed })?;
            return R{ .ok = v };
        };
        pub const main := fn(): i32 {
            var counter: i32 = 0;
            var seed: i32 = -1;
            _ = match (inner(seed, ^mut counter)) {
                .ok => |v| v,
                .err => |e| e,
            };
            return counter;
        };
    )") == 10);
}

TEST_CASE("`?` propagates an Optional's none out of the enclosing function") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const O := Option(i32);
        const first_positive := fn(a: i32): O {
            return if (a > 0) O{ .some = a }; else O{ .none = {} };
        };
        const add_one := fn(a: i32): O {
            const v := first_positive(a)?;
            return O{ .some = v + 1 };
        };
        pub const main := fn(): i32 {
            var a: i32 = 0;
            return match (add_one(a)) {
                .some => |v| v,
                .none => 7,
            };
        };
    )") == 7);
}

TEST_CASE("`!` projects the payload of the active variant") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const O := Option(i32);
        const grab := fn(o: O): i32 { return o!; };
        pub const main := fn(): i32 {
            var n: i32 = 7;
            return grab(O{ .some = n }) + 1;
        };
    )") == 8);

    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, bool);
        const unwrap_it := fn(r: R): i32 { return r!; };
        pub const main := fn(): i32 {
            var n: i32 = 41;
            return unwrap_it(R{ .ok = n }) + 1;
        };
    )") == 42);
}

TEST_CASE("`?` composes across call layers") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const a := fn(x: i32): R { return if (x == 0) R{ .err = 1 }; else R{ .ok = x }; };
        const b := fn(x: i32): R { const v := a(x)?; return R{ .ok = v + 1 }; };
        const c := fn(x: i32): R { const v := b(x)?; return R{ .ok = v + 1 }; };
        pub const main := fn(): i32 {
            var x: i32 = 40;
            return match (c(x)) { .ok => |v| v, .err => |e| e };
        };
    )") == 42);
}

TEST_CASE("nominal `?` and `!` work on custom renamed-variant unions") {
    CHECK(helpers::compile_and_run(R"(
        const Custom := union { item: i32, failure: u8 };
        impl builtin.Unwrappable for Custom {
            using Output = i32;
            using Residual = u8;
            pub const isBreak := fn(&self): bool {
                return match (self) {
                    .item => false,
                    .failure => true,
                };
            };
            pub const intoOutput := fn(self): i32 {
                return match (self) {
                    .item => |v| v,
                    .failure => @trap(),
                };
            };
            pub const intoResidual := fn(self): u8 {
                return match (self) {
                    .failure => |e| e,
                    .item => @trap(),
                };
            };
        }
        impl builtin.Rewrappable for Custom {
            using From = u8;
            pub const fromResidual := fn(r: u8): @this() {
                return .{ .failure = r };
            };
        }
        const step := fn(x: i32): Custom {
            return if (x > 10) Custom{ .item = x * 2 }; else Custom{ .failure = 7u8 };
        };
        const run := fn(x: i32): Custom {
            const v := step(x)?;
            return Custom{ .item = v + 1 };
        };
        pub const main := fn(): i32 {
            const good := run(20);
            const val := good!;
            const bad := run(5);
            const err_code := match (bad) {
                .item => 0,
                .failure => |e| @intCast(i32, e),
            };
            return val + err_code; // 41 + 7 = 48
        };
    )") == 48);
}

} // namespace ghoti::tests
