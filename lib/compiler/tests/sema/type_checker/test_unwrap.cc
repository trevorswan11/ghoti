#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

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
)";

} // namespace

TEST_CASE("`?` type-checks the lowered Result propagation") {
    SECTION("Matching error payloads pass the type check") {
        helpers::type_check_and_verify(std::string{RESULT_PRELUDE} + R"(
const R := Result(i32, i32);
const inner := fn(): R { return R{ .ok = 1 }; };
const outer := fn(): R {
    const v := inner()?;
    return R{ .ok = v };
};
)");
    }
}

} // namespace ghoti::tests
