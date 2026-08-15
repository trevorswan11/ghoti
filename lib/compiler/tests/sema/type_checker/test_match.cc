#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Mutating a plain match arm capture") {
    SECTION("Through a var union succeeds") {
        helpers::type_check_and_verify(R"(
            const U := union { a: i32 };
            const f := fn(): void {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |v| { v = 10; },
                    _ => {},
                };
            };
        )");
    }

    SECTION("Through a const union is rejected") {
        helpers::test_checker_fail(
            R"(
            const U := union { a: i32 };
            const f := fn(): void {
                const u := U{ .a = 5 };
                match (u) {
                    .a => |v| { v = 10; },
                    _ => {},
                };
            };
        )",
            sema::diagnostic{"Cannot assign to an element of a non-mutable array or slice",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{5UZ, 36UZ}});
    }

    SECTION("Through a var whole-value (non-union) capture succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                var x: i32 = 1;
                match (x) {
                    1 => |v| { v = 2; },
                    _ => {},
                };
            };
        )");
    }

    SECTION("Through a const whole-value (non-union) capture is rejected") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                const x: i32 = 1;
                match (x) {
                    1 => |v| { v = 2; },
                    _ => {},
                };
            };
        )",
            sema::diagnostic{"Cannot assign to constant variable",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{4UZ, 35UZ}});
    }
}

TEST_CASE("Mutating a match arm capture by reference/pointer") {
    SECTION("By mutable reference succeeds") {
        helpers::type_check_and_verify(R"(
            const U := union { a: i32 };
            const f := fn(): void {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |&mut v| { v = 10; },
                    _ => {},
                };
            };
        )");
    }

    SECTION("By const reference is rejected") {
        helpers::test_checker_fail(
            R"(
            const U := union { a: i32 };
            const f := fn(): void {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |&v| { v = 10; },
                    _ => {},
                };
            };
        )",
            sema::diagnostic{"Cannot assign to constant memory through reference",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{5UZ, 37UZ}});
    }

    SECTION("By mutable pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const U := union { a: i32 };
            const f := fn(): void {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |^mut v| { *v = 10; },
                    _ => {},
                };
            };
        )");
    }

    SECTION("By const pointer is rejected") {
        helpers::test_checker_fail(
            R"(
            const U := union { a: i32 };
            const f := fn(): void {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |^v| { *v = 10; },
                    _ => {},
                };
            };
        )",
            sema::diagnostic{"Cannot assign to constant memory through pointer",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{5UZ, 38UZ}});
    }
}

} // namespace ghoti::tests
