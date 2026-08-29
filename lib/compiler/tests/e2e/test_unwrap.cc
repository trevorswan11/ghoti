#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`?` yields the ok payload and lets execution continue") {
    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: i32 };
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
    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: i32 };
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
    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: i32 };
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
    CHECK(helpers::compile_and_run(R"(
        const O := union { some: i32, none: void };
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
    CHECK(helpers::compile_and_run(R"(
        const O := union { some: i32, none: void };
        const grab := fn(o: O): i32 { return o!; };
        pub const main := fn(): i32 {
            var n: i32 = 7;
            return grab(O{ .some = n }) + 1;
        };
    )") == 8);

    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: bool };
        const unwrap_it := fn(r: R): i32 { return r!; };
        pub const main := fn(): i32 {
            var n: i32 = 41;
            return unwrap_it(R{ .ok = n }) + 1;
        };
    )") == 42);
}

TEST_CASE("`?` composes across call layers") {
    CHECK(helpers::compile_and_run(R"(
        const R := union { ok: i32, err: i32 };
        const a := fn(x: i32): R { return if (x == 0) R{ .err = 1 }; else R{ .ok = x }; };
        const b := fn(x: i32): R { const v := a(x)?; return R{ .ok = v + 1 }; };
        const c := fn(x: i32): R { const v := b(x)?; return R{ .ok = v + 1 }; };
        pub const main := fn(): i32 {
            var x: i32 = 40;
            return match (c(x)) { .ok => |v| v, .err => |e| e };
        };
    )") == 42);
}

} // namespace ghoti::tests
