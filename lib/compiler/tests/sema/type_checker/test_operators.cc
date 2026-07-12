#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Operator type checking") {
    SECTION("Valid binary arithmetic succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(a: i32, b: i32): i32 {
                return a + b;
            };
        )");
    }

    SECTION("Adding booleans fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: bool, b: bool): void {
                const x := a + b;
            };
        )",
            sema::diagnostic{"Operator 'add' cannot be applied to types 'bool' and 'bool'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 32UZ}});
    }

    SECTION("Mismatched numeric types in addition without cast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: i32, b: f32): void {
                const x := a + b;
            };
        )",
            sema::diagnostic{"Operator 'add' cannot be applied to types 'i32' and 'f32'",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 32UZ}});
    }

    SECTION("Logical negation on non-boolean fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: i32): void {
                const x := !a;
            };
        )",
            sema::diagnostic{"Logical negation '!' requires a boolean operand",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 29UZ}});
    }

    SECTION("Unary negation on unsigned integer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: u32): void {
                const x := -a;
            };
        )",
            sema::diagnostic{"Unary negation '-' requires a signed integer or float operand",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 29UZ}});
    }

    SECTION("Bitwise negation on float fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(a: f32): void {
                const x := ~a;
            };
        )",
            sema::diagnostic{"Bitwise negation '~' requires an integer operand",
                             sema::error::OPERATOR_TYPE_MISMATCH,
                             std::pair{2UZ, 29UZ}});
    }
}

} // namespace ghoti::tests
