#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`= undefined` leaves a scalar local uninitialized but usable once written") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = undefined;
            x = 42;
            return x;
        };
    )") == 42);
}

TEST_CASE("`= undefined` allocates an aggregate local without a store") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32, y: i32 };
        pub const main := fn(): i32 {
            var p: Point = undefined;
            p.x = 40;
            p.y = 2;
            return p.x + p.y;
        };
    )") == 42);
}

TEST_CASE("`= undefined` works for an array local written through mutable elements") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var buf: [3uz]mut i32 = undefined;
            buf[0] = 10;
            buf[1] = 20;
            buf[2] = 12;
            return buf[0] + buf[1] + buf[2];
        };
    )") == 42);
}

} // namespace ghoti::tests
