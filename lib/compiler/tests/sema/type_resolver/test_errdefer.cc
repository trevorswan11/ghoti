#include <algorithm>
#include <string>
#include <string_view>

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

auto check_has_sema_error(std::string_view code, sema::error expected_error) -> void {
    CHECK(std::ranges::contains(helpers::resolver_error_codes(code), expected_error));
}

} // namespace

TEST_CASE("errdefer in infallible function is rejected") {
    check_has_sema_error("const f := fn(): i32 { errdefer {} return 0; };",
                         sema::error::ERRDEFER_IN_INFALLIBLE_FN);

    check_has_sema_error("const f := fn(): void { errdefer {} };",
                         sema::error::ERRDEFER_IN_INFALLIBLE_FN);
}

TEST_CASE("errdefer with mutable capture is rejected") {
    check_has_sema_error(std::string{RESULT_PRELUDE} + R"(
        const f := fn(): Result(i32, i32) {
            errdefer |&mut e| {}
            return .{ .ok = 1 };
        };
        )",
                         sema::error::ERRDEFER_MUTABLE_CAPTURE);

    check_has_sema_error(std::string{RESULT_PRELUDE} + R"(
        const f := fn(): Result(i32, i32) {
            errdefer |^mut e| {}
            return .{ .ok = 1 };
        };
        )",
                         sema::error::ERRDEFER_MUTABLE_CAPTURE);
}

TEST_CASE("errdefer body jump rejection") {
    check_has_sema_error(std::string{RESULT_PRELUDE} + R"(
        const f := fn(): Result(i32, i32) {
            errdefer { return; }
            return .{ .ok = 1 };
        };
        )",
                         sema::error::DEFER_BODY_JUMP);

    check_has_sema_error(std::string{RESULT_PRELUDE} + R"(
        const f := fn(): Result(i32, i32) {
            errdefer { break; }
            return .{ .ok = 1 };
        };
        )",
                         sema::error::DEFER_BODY_JUMP);

    check_has_sema_error(std::string{RESULT_PRELUDE} + R"(
        const f := fn(): Result(i32, i32) {
            errdefer { continue; }
            return .{ .ok = 1 };
        };
        )",
                         sema::error::DEFER_BODY_JUMP);
}

TEST_CASE("errdefer capture typing") {
    SECTION("by-value capture receives error payload type") {
        auto [ctx, idx]{helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
            const f := fn(): Result(i32, bool) {
                errdefer |e| {
                    const check: bool = e;
                }
                return .{ .ok = 1 };
            };
        )")};
    }

    SECTION("alias capture receives const reference / pointer type") {
        auto [ctx, idx]{helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
            const f := fn(): Result(i32, bool) {
                errdefer |&e| {
                    const check: &bool = e;
                }
                errdefer |^p| {
                    const check_p: ^bool = p;
                }
                return .{ .ok = 1 };
            };
        )")};
    }

    SECTION("discard capture is allowed") {
        auto [ctx, idx]{helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
            const f := fn(): Result(i32, i32) {
                errdefer |_| {}
                return .{ .ok = 1 };
            };
        )")};
    }

    SECTION("Option return type captures void") {
        auto [ctx, idx]{helpers::resolve_and_check(std::string{RESULT_PRELUDE} + R"(
            const f := fn(): Option(i32) {
                errdefer |e| {
                    const v: void = e;
                }
                return .{ .some = 1 };
            };
        )")};
    }
}

} // namespace ghoti::tests
