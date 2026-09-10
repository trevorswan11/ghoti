#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

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

TEST_CASE("Resolving well-formed `?` propagation") {
    SECTION("`?` yields the `ok` payload, enclosing function returns a matching Result") {
        helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, i32);
        const g := fn(): R { return R{ .ok = 1 }; };
        const f := fn(): R { const v := g()?; return R{ .ok = v }; };
    )");
    }

    SECTION("`?` yields the `some` payload, enclosing function returns a matching Optional") {
        helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
        const O := Option(i32);
        const g := fn(): O { return O{ .some = 1 }; };
        const f := fn(): O { const v := g()?; return O{ .some = v }; };
    )");
    }

    SECTION("Structurally valid: same family, and the two `ok` payload types need not match") {
        helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
        const E := Result(i32, i32);
        const F := Result(bool, i32);
        const inner := fn(): E { return E{ .ok = 1 }; };
        const outer := fn(): F { const v := inner()?; _ = v; return F{ .ok = true }; };
    )");
    }
}

TEST_CASE("Resolving well-formed `!` assert-unwrap") {
    helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
        const R := Result(i32, bool);
        const f := fn(r: R): i32 { return r!; };
    )");

    helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
        const O := Option(i32);
        const f := fn(o: O): i32 { return o!; };
    )");
}

TEST_CASE("`?` / `!` on a non-Result operand is rejected") {
    helpers::test_resolver_fail(
        "const f := fn(x: i32): i32 { x?; };",
        sema::diagnostic{
            "the postfix '?' operator expects a type implementing 'builtin.Unwrappable'; "
            "'i32' does not implement it",
            sema::error::UNWRAP_ON_NON_RESULT,
            std::pair{0UZ, 29UZ}});

    helpers::test_resolver_fail(
        "const f := fn(x: i32): i32 { x!; };",
        sema::diagnostic{
            "the postfix '!' operator expects a type implementing 'builtin.Unwrappable'; "
            "'i32' does not implement it",
            sema::error::UNWRAP_ON_NON_RESULT,
            std::pair{0UZ, 29UZ}});

    helpers::test_resolver_fail(
        R"(
const U := union { a: i32, b: i32 };
const f := fn(u: U): i32 { return u!; };
)",
        sema::diagnostic{
            "the postfix '!' operator expects a type implementing 'builtin.Unwrappable'; "
            "'U' does not implement it; add 'impl builtin.Unwrappable for U'",
            sema::error::UNWRAP_ON_NON_RESULT,
            std::pair{2UZ, 34UZ}});
}

TEST_CASE("`?` outside a function is rejected") {
    helpers::test_resolver_fail(
        std::string{RESULT_PRELUDE} + R"(
const R := Result(i32, i32);
const r := R{ .ok = 1 };
const b := r?;
)",
        sema::diagnostic{"the '?' operator can only be used inside a function",
                         sema::error::UNWRAP_OUTSIDE_FUNCTION,
                         std::pair{34UZ, 11UZ}});
}

TEST_CASE("`?` requires the enclosing function to return a matching Result / Optional") {
    SECTION("Enclosing function does not implement Rewrappable") {
        helpers::test_resolver_fail(
            std::string{RESULT_PRELUDE} + R"(
const R := Result(i32, i32);
const f := fn(r: R): i32 {
    return r?;
};
)",
            sema::diagnostic{"the '?' operator propagates a 'union' residual ('i32') but 'i32' "
                             "does not implement 'builtin.Rewrappable'",
                             sema::error::UNWRAP_RETURN_TYPE_MISMATCH,
                             std::pair{34UZ, 11UZ}});
    }

    SECTION("Optional `?` inside a Result-returning function: From mismatch") {
        helpers::test_resolver_fail(
            std::string{RESULT_PRELUDE} + R"(
const O := Option(i32);
const R := Result(i32, i32);
const f := fn(o: O): R {
    return o?;
};
)",
            sema::diagnostic{"the '?' operator propagates a 'union' residual ('void') but 'union' "
                             "is not rebuildable from 'void'; implement 'builtin.Rewrappable for "
                             "union' with From = 'void'",
                             sema::error::UNWRAP_RETURN_TYPE_MISMATCH,
                             std::pair{35UZ, 11UZ}});
    }
}

TEST_CASE("Nominal Unwrappable and Rewrappable resolution") {
    SECTION("Custom union implementing Unwrappable and Rewrappable") {
        helpers::resolve_and_check(R"(
        const MyRes := union { val: i32, fail: u8 };
        impl builtin.Unwrappable for MyRes {
            using Output = i32;
            using Residual = u8;
            pub const branch := fn(self): builtin.Flow(i32, u8) {
                return match (self) {
                    .val => |v| builtin.Flow(i32, u8){ .@"continue" = v },
                    .fail => |f| builtin.Flow(i32, u8){ .@"break" = f },
                };
            };
        }
        impl builtin.Rewrappable for MyRes {
            using From = u8;
            pub const fromResidual := fn(r: u8): @this() { return MyRes{ .fail = r }; };
        }
        const f := fn(m: MyRes): MyRes {
            const v := m?;
            return MyRes{ .val = v };
        };
        const g := fn(m: MyRes): i32 {
            return m!;
        };
        )");
    }

    SECTION("Cross-function widening of residual") {
        helpers::resolve_and_check(R"(
        const MyRes := union { val: i32, fail: u8 };
        impl builtin.Unwrappable for MyRes {
            using Output = i32;
            using Residual = u8;
            pub const branch := fn(self): builtin.Flow(i32, u8) {
                return match (self) {
                    .val => |v| builtin.Flow(i32, u8){ .@"continue" = v },
                    .fail => |f| builtin.Flow(i32, u8){ .@"break" = f },
                };
            };
        }
        const BigRes := union { val: i32, fail: u32 };
        impl builtin.Rewrappable for BigRes {
            using From = u32;
            pub const fromResidual := fn(r: u32): @this() { return BigRes{ .fail = r }; };
        }
        const f := fn(m: MyRes): BigRes {
            const v := m?;
            return BigRes{ .val = v };
        };
        )");
    }

    SECTION("Narrowing residual is rejected with cast rejection reason") {
        helpers::test_resolver_fail(
            R"(
const BigRes := union { val: i32, fail: u32 };
impl builtin.Unwrappable for BigRes {
    using Output = i32;
    using Residual = u32;
    pub const branch := fn(self): builtin.Flow(i32, u32) {
        return match (self) {
            .val => |v| builtin.Flow(i32, u32){ .@"continue" = v },
            .fail => |f| builtin.Flow(i32, u32){ .@"break" = f },
        };
    };
}
const SmallRes := union { val: i32, fail: u8 };
impl builtin.Rewrappable for SmallRes {
    using From = u8;
    pub const fromResidual := fn(r: u8): @this() { return SmallRes{ .fail = r }; };
}
const f := fn(m: BigRes): SmallRes {
    return m?;
};
)",
            sema::diagnostic{
                "the '?' operator propagates a 'BigRes' residual ('u32') but 'SmallRes' is not "
                "rebuildable from 'u32' (narrowing conversion from 'u32' to 'u8' may truncate high "
                "bits; "
                "use @intCast for a checked conversion or @truncate to discard high bits); "
                "implement 'builtin.Rewrappable for SmallRes' with From = 'u32'",
                sema::error::UNWRAP_RETURN_TYPE_MISMATCH,
                std::pair{18UZ, 11UZ}});
    }
}

} // namespace ghoti::tests
