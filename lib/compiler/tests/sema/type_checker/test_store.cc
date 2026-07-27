#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Type checker - store and assignment validation") {
    SECTION("Valid variable assignment succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                var x: i32 = 0;
                x = 42;
            };
        )");
    }

    SECTION("Assignment with incompatible type fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: i32 = 0;
                x = true;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'bool' to 'i32'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{3UZ, 20UZ}});
    }

    SECTION("Store through mutable pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^mut i32): void {
                *p = 42;
            };
        )");
    }

    SECTION("Store through const pointer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^i32): void {
                *p = 42;
            };
        )",
            sema::diagnostic{"Cannot assign to constant memory through pointer",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{2UZ, 21UZ}});
    }

    SECTION("Store incompatible type through pointer fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^mut i32): void {
                *p = true;
            };
        )",
            sema::diagnostic{"Type mismatch in store: cannot assign 'bool' to 'i32'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{2UZ, 21UZ}});
    }

    SECTION("Allocating opaque variable fails with ILLEGAL_OPAQUE_TYPE") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                var x: opaque = undefined;
            };
        )",
            sema::diagnostic{"Cannot allocate variable of opaque type",
                             sema::error::ILLEGAL_OPAQUE_TYPE,
                             std::pair{2UZ, 16UZ}});
    }
}

} // namespace ghoti::tests
