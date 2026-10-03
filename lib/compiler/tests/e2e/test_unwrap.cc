#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view RESULT_PRELUDE = R"(
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
impl(T: type, E: type) builtin.Rewrappable for Result(T, E) {
    const From = E;
    pub const from_residual = fn(r: E): @This() { return .{ .err = r }; };
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
impl(T: type) builtin.Rewrappable for Option(T) {
    const From = void;
    pub const from_residual = fn(_: void): @This() { return .{ .none = {} }; };
}
)";

} // namespace

TEST_CASE("`?` and `!` fold at compile time through places, temporaries, and references") {
    const auto defs{std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const half = fn(n: i32): R { return if (n % 2 == 0) R{ .ok = n / 2 } else R{ .err = 7 }; };
        const chain = fn(n: i32): R { let h = half(n)?; let q = half(h)?; return R{ .ok = q + 100 }; };
        const value = fn(r: R): i32 { return match (r) { .ok => |v| v, .err => |e| e }; };
        const t = fn(): i32 {
            let r = half(10);
            let by_ref = &r;
            return value(chain(8)) + value(chain(6)) + r! + by_ref! + half(4)!;
        };
    )"};
    // 102 + 7 + 5 + 5 + 2
    CHECK(helpers::compile_and_run(defs + "pub const main = fn(): i32 { return t(); };") == 121);
    CHECK(helpers::compile_and_run(defs + "pub const main = fn(): i32 { return comptime t(); };") ==
          121);
}

TEST_CASE("`?` yields the ok payload and lets execution continue") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const parse = fn(x: i32): R {
            return if (x < 0) R{ .err = 99 }; else R{ .ok = x + 1 };
        };
        const doubled = fn(x: i32): R {
            let v = parse(x)?;
            return R{ .ok = v * 2 };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = 20;
            return match (doubled(x)) {
                .ok => |v| v,
                .err => |e| e,
            };
        };
    )") == 42);
}

TEST_CASE("`?` on the err variant propagates out of the enclosing function") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const parse = fn(x: i32): R {
            return if (x < 0) R{ .err = 99 }; else R{ .ok = x + 1 };
        };
        const doubled = fn(x: i32): R {
            let v = parse(x)?;
            return R{ .ok = v * 2 };
        };
        pub const main = fn(): i32 {
            let mut x: i32 = -5;
            return match (doubled(x)) {
                .ok => |v| v,
                .err => |e| e,
            };
        };
    )") == 99);
}

TEST_CASE("`?` runs enclosing scope defers on the propagation path") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const inner = fn(seed: i32, log: ^mut i32): R {
            defer *log = *log + 10;
            let v = (if (seed < 0) R{ .err = 5 }; else R{ .ok = seed })?;
            return R{ .ok = v };
        };
        pub const main = fn(): i32 {
            let mut counter: i32 = 0;
            let mut seed: i32 = -1;
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
        const O = Option(i32);
        const first_positive = fn(a: i32): O {
            return if (a > 0) O{ .some = a }; else O{ .none = {} };
        };
        const add_one = fn(a: i32): O {
            let v = first_positive(a)?;
            return O{ .some = v + 1 };
        };
        pub const main = fn(): i32 {
            let mut a: i32 = 0;
            return match (add_one(a)) {
                .some => |v| v,
                .none => 7,
            };
        };
    )") == 7);
}

TEST_CASE("`!` projects the payload of the active variant") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const O = Option(i32);
        const grab = fn(o: O): i32 { return o!; };
        pub const main = fn(): i32 {
            let mut n: i32 = 7;
            return grab(O{ .some = n }) + 1;
        };
    )") == 8);

    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, bool);
        const unwrap_it = fn(r: R): i32 { return r!; };
        pub const main = fn(): i32 {
            let mut n: i32 = 41;
            return unwrap_it(R{ .ok = n }) + 1;
        };
    )") == 42);
}

TEST_CASE("`?` composes across call layers") {
    CHECK(helpers::compile_and_run(std::string{RESULT_PRELUDE} + R"(
        const R = Result(i32, i32);
        const a = fn(x: i32): R { return if (x == 0) R{ .err = 1 }; else R{ .ok = x }; };
        const b = fn(x: i32): R { let v = a(x)?; return R{ .ok = v + 1 }; };
        const c = fn(x: i32): R { let v = b(x)?; return R{ .ok = v + 1 }; };
        pub const main = fn(): i32 {
            let mut x: i32 = 40;
            return match (c(x)) { .ok => |v| v, .err => |e| e };
        };
    )") == 42);
}

TEST_CASE("nominal `?` and `!` work on custom renamed-variant unions") {
    CHECK(helpers::compile_and_run(R"(
        const Custom = union { item: i32, failure: u8 };
        impl builtin.Unwrappable for Custom {
            const Output = i32;
            const Residual = u8;
            pub const branch = fn(&mut? self): builtin.Flow(&mut? i32, u8) {
                return match (self) {
                    .item => |&mut? v| .{ .@"continue" = v },
                    .failure => |e| .{ .@"break" = e },
                };
            };
        }
        impl builtin.Rewrappable for Custom {
            const From = u8;
            pub const from_residual = fn(r: u8): @This() {
                return .{ .failure = r };
            };
        }
        const step = fn(x: i32): Custom {
            return if (x > 10) Custom{ .item = x * 2 }; else Custom{ .failure = 7u8 };
        };
        const run = fn(x: i32): Custom {
            let v = step(x)?;
            return Custom{ .item = v + 1 };
        };
        pub const main = fn(): i32 {
            let good = run(20);
            let val = good!;
            let bad = run(5);
            let err_code = match (bad) {
                .item => 0,
                .failure => |e| @intCast(i32, e),
            };
            return val + err_code; // 41 + 7 = 48
        };
    )") == 48);
}

} // namespace ghoti::tests
