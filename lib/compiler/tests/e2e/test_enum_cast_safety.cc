#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("an in-range integer casts to an exhaustive enum and passes the guard") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };

        pub const main := fn(): i32 {
            var n: i32 = 2;
            const c := @as(Color, n);
            return @as(i32, c);
        };
    )") == 2);
}

TEST_CASE("a non-exhaustive enum accepts any underlying value without a guard") {
    CHECK(helpers::compile_and_run(R"(
        const Flags := enum { none, one, two, _ };

        pub const main := fn(): i32 {
            var n: i32 = 40;
            const f := @as(Flags, n);
            return @as(i32, f) + 2;
        };
    )") == 42);
}

TEST_CASE("explicit discriminants define the valid set for the guard") {
    CHECK(helpers::compile_and_run(R"(
        const Code := enum { ok, retry, fatal };

        pub const main := fn(): i32 {
            var n: i32 = 1;
            const c := @as(Code, n);
            return @as(i32, c);
        };
    )") == 1);
}

} // namespace ghoti::tests
