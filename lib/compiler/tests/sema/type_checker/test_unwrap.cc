#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`?` type-checks the lowered Result propagation") {
    SECTION("Matching error payloads pass the store check") {
        helpers::type_check_and_verify(R"(
const R := union { ok: i32, err: i32 };
const inner := fn(): R { return R{ .ok = 1 }; };
const outer := fn(): R {
    const v := inner()?;
    return R{ .ok = v };
};
)");
    }

    SECTION("An error payload not assignable to the enclosing error type is a store mismatch") {
        helpers::test_checker_fail(
            R"(
const R1 := union { ok: i32, err: i32 };
const R2 := union { ok: i32, err: bool };
const f := fn(r: R1): R2 {
    const v := r?;
    return R2{ .ok = v };
};
)",
            sema::diagnostic{"Type mismatch in store: cannot assign 'i32' to 'bool'",
                             sema::error::TYPE_MISMATCH,
                             std::pair{4UZ, 15UZ}});
    }
}

} // namespace ghoti::tests
