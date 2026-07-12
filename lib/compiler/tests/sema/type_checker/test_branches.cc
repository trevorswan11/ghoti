#include <catch2/catch_test_macros.hpp>

#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Branch and control flow type checking") {
    SECTION("Valid if condition succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(cond: bool): i32 {
                if (cond) {
                    return 1;
                } else {
                    return 2;
                }
            };
        )");
    }

    SECTION("Valid while loop condition succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): i32 {
                var i: i32 = 0;
                while (i < 10) {
                    i += 1;
                }
                return i;
            };
        )");
    }
}

} // namespace ghoti::tests
