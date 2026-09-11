#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// Phase 11: `@hasField`/`@fieldType`. `@field` is scaffolded (token/builtin registered, arity/
// type-checked) but gated to a clean `FIELD_NOT_FOUND` "not yet implemented" error - its full
// semantics (data-field GEP + lvalue, static/const, bound method) are a follow-up; see the
// design doc §9.2 (referenced from §14 Phase 11).
TEST_CASE("`@hasField` reports whether a struct has a data field") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            return @intFromBool(@hasField(Point, "x")) + @intFromBool(@hasField(Point, "z")) * 10;
        };
    )") == 1);
}

TEST_CASE("`@fieldType` returns a struct field's declared type") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            return match constexpr (@typeInfo(@fieldType(Point, "x"))) {
                .int => |i| @intCast(i32, i.bits),
                _ => 0,
            };
        };
    )") == 32);
}

} // namespace ghoti::tests
