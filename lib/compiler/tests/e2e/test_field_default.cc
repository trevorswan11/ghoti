#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`@fieldDefault` returns a defaulted field's compile-time value") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct {
            x: i32 = 5,
            y: i32,
        };
        pub const main := fn(): i32 { return @fieldDefault(Point, "x"); };
    )") == 5);
}

TEST_CASE("`@fieldDefault` rejects a field with no default") {
    helpers::expect_compile_error(R"(
        const Point := struct {
            x: i32 = 5,
            y: i32,
        };
        pub const main := fn(): i32 { return @fieldDefault(Point, "y"); };
    )");
}

TEST_CASE("`@fieldDefault` rejects an unknown field name") {
    helpers::expect_compile_error(R"(
        const Point := struct {
            x: i32 = 5,
        };
        pub const main := fn(): i32 { return @fieldDefault(Point, "z"); };
    )");
}

} // namespace ghoti::tests
