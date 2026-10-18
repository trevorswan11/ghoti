#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A later global is typed on its own, not by the `return` that first reads it") {
    CHECK(helpers::compile_and_run(R"(
        const as_float = fn(): f32 { return N; };
        const N = 3;
        const as_int = fn(): u32 { let x: u32 = N; return x; };
        pub const main = fn(): i32 {
            return @intFromFloat(i32, as_float()) + @intCast(i32, as_int());
        };
    )") == 6);
}

TEST_CASE("A callee resolved from a caller's `return` keeps its own local constant types") {
    CHECK(helpers::compile_and_run(R"(
        const wrap = fn(p: f32): f32 { return callee(p); };
        const callee = fn(a: f32): f32 {
            const m = 255;
            let x: u32 = @bitCast(u32, a) & m;
            return a + @floatFromInt(f32, x);
        };
        pub const main = fn(): i32 {
            return @intFromFloat(i32, wrap(1.0));
        };
    )") == 1);
}

} // namespace ghoti::tests
