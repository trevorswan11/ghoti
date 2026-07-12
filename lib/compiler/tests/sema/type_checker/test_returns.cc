#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Return statement type checking") {
    SECTION("Valid return in typed function succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): i32 {
                return 42;
            };
        )");
    }

    SECTION("Valid empty return in void function succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(): void {
                return;
            };
        )");
    }

    SECTION("Implicit widening return succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(x: u8): u32 {
                return x;
            };
        )");
    }

    SECTION("Returning value from void function fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): void {
                return 42;
            };
        )",
            sema::diagnostic{"Cannot return a value from a function returning void",
                             sema::error::RETURN_TYPE_MISMATCH,
                             std::pair{2UZ, 24UZ}});
    }

    SECTION("Returning wrong type from typed function fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(): i32 {
                return true;
            };
        )",
            sema::diagnostic{
                "Return value of type 'bool' is not assignable to function return type 'i32'",
                sema::error::RETURN_TYPE_MISMATCH,
                std::pair{2UZ, 24UZ}});
    }

    SECTION("Conflicting return types in auto function runtime branches fails in Pass 4") {
        helpers::test_checker_fail(
            R"(
            const f := fn(c: bool): auto {
                if (c) {
                    return 1;
                } else {
                    return true;
                }
            };
        )",
            sema::diagnostic{
                "Return value of type 'bool' is not assignable to function return type 'i32'",
                sema::error::RETURN_TYPE_MISMATCH,
                std::pair{5UZ, 28UZ}});
    }

    SECTION("Constexpr multi-type if branches succeed") {
        helpers::type_check_and_verify(R"(
            const f := fn(): auto {
                if constexpr (true) {
                    return 42;
                } else {
                    return true;
                }
            };
        )");
    }
}

} // namespace ghoti::tests
