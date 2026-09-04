#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
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

    SECTION("Valid match expression arms succeed") {
        helpers::type_check_and_verify(R"(
            const f := fn(x: i32): i32 {
                const y: i32 = match (x) {
                    1 => 10,
                    2 => 20,
                    _ => 30,
                };
                return y;
            };
        )");
    }

    SECTION("Match expression with unreachable arm succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(x: i32): i32 {
                const y: i32 = match (x) {
                    1 => 10,
                    _ => unreachable,
                };
                return y;
            };
        )");
    }

    SECTION("Match expression with mismatched arm types fails in type checker") {
        helpers::test_checker_fail(
            R"(
            const f := fn(x: i32): i32 {
                const y := match (x) {
                    1 => 10,
                    _ => true,
                };
                return y;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'bool' to 'constexpr_int'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{4UZ, 25UZ}});
    }
}

} // namespace ghoti::tests
