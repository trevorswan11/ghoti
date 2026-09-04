#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Well-formed integer / float builtins resolve") {
    helpers::resolve_and_check(R"(
        const f := fn(a: i32, b: i32): i32 { return @min(a, b) + @max(a, b); };
        const g := fn(a: f64, b: f64): f64 { return @min(a, b); };
        const h := fn(a: i32, b: i32): i32 {
            return @divTrunc(a, b) + @divFloor(a, b) + @rem(a, b) + @mod(a, b);
        };
        const k := fn(a: u32, b: u32): bool {
            var out: u32 = 0u32;
            return @addWithOverflow(a, b, &mut out);
        };
        const p := fn(a: u32, b: u32): bool {
            var out: u32 = 0u32;
            return @mulWithOverflow(a, b, ^mut out); // a `^mut T` pointer is also accepted
        };
    )");
}

TEST_CASE("@min / @max reject operands of differing type") {
    helpers::test_resolver_fail(
        R"(
const foo := fn(a: i32, b: i64): i32 {
    return @min(a, b);
};
)",
        sema::diagnostic{"'@min' expects two operands of the same numeric type; found 'i32' and "
                         "'i64'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{2UZ, 16UZ}});
}

TEST_CASE("@divTrunc rejects floating-point operands") {
    helpers::test_resolver_fail(
        R"(
const foo := fn(a: f64, b: f64): f64 {
    return @divTrunc(a, b);
};
)",
        sema::diagnostic{"'@divTrunc' expects two operands of the same integer type; found 'f64' "
                         "and 'f64'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{2UZ, 21UZ}});
}

TEST_CASE("@addWithOverflow rejects a by-value result argument") {
    helpers::test_resolver_fail(
        R"(
const foo := fn(a: i32, b: i32): bool {
    var out: i32 = 0;
    return @addWithOverflow(a, b, out);
};
)",
        sema::diagnostic{"'@addWithOverflow' expects its third argument to be a '&mut i32' result "
                         "reference; found 'i32'",
                         sema::error::TYPE_MISMATCH,
                         std::pair{3UZ, 34UZ}});
}

TEST_CASE("Compile-time division / modulo by zero is an error") {
    helpers::test_checker_fail(
        "const bad := @divTrunc(10, 0);",
        sema::diagnostic{"Division by zero in compile-time constant expression",
                         sema::error::CONSTEXPR_EVALUATION_FAILED,
                         std::pair{0UZ, 22UZ}});

    helpers::test_checker_fail(
        "const bad := @mod(10, 0);",
        sema::diagnostic{"Modulo by zero in compile-time constant expression",
                         sema::error::CONSTEXPR_EVALUATION_FAILED,
                         std::pair{0UZ, 17UZ}});
}

} // namespace ghoti::tests
