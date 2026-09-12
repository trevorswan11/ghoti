#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("An enum's underlying type may be a call expression that produces a type") {
    CHECK(helpers::compile_and_run(R"(
        const Identity := fn(T: type): type { return T; };
        const Color := enum : Identity(i32) { red, green = 5, blue };
        pub const main := fn(): i32 {
            return @as(i32, Color.green);
        };
    )") == 5);
}

TEST_CASE("An enum's underlying type still accepts a plain identifier") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum : u8 { red, green = 5, blue };
        pub const main := fn(): i32 {
            return @intCast(i32, @as(u8, Color.green));
        };
    )") == 5);
}

} // namespace ghoti::tests
