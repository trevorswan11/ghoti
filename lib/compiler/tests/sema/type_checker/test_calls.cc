#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Function call type checking") {
    SECTION("Valid call with exact argument types succeeds") {
        helpers::type_check_and_verify(R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10, 20);
            };
        )");
    }

    SECTION("Valid call with implicitly widened argument succeeds") {
        helpers::type_check_and_verify(R"(
            const take_u32 := fn(x: u32): u32 {
                return x;
            };
            const f := fn(b: u8): u32 {
                return take_u32(b);
            };
        )");
    }

    SECTION("Call with wrong argument count fails with ARITY_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10);
            };
        )",
            sema::diagnostic{"Expected 2 arguments, found 1",
                             sema::error::ARITY_MISMATCH,
                             std::pair{5UZ, 24UZ}});
    }

    SECTION("Call with incompatible argument type fails with TYPE_MISMATCH") {
        helpers::test_checker_fail(
            R"(
            const add := fn(a: i32, b: i32): i32 {
                return a + b;
            };
            const f := fn(): i32 {
                return add(10, true);
            };
        )",
            sema::diagnostic{"Argument 2 of type 'bool' is not assignable to parameter type 'i32' "
                             "in call to 'add'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{5UZ, 32UZ}});
    }
}

} // namespace ghoti::tests
