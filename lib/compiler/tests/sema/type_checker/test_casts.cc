#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Cast type checking") {
    SECTION("Valid pointer cast succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^mut i32): ^i32 {
                return @ptrCast(^i32, p);
            };
        )");
    }

    SECTION("Valid const pointer cast succeeds") {
        helpers::type_check_and_verify(R"(
            const f := fn(p: ^i32): ^mut i32 {
                return @constCast(p);
            };
        )");
    }

    SECTION("Casting away const from pointer without constCast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^i32): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )",
            sema::diagnostic{"Cannot cast away const from pointer without @constCast",
                             sema::error::ILLEGAL_CONST_CAST,
                             std::pair{2UZ, 42UZ}});
    }

    SECTION("Casting between typed pointer and opaque pointer succeeds") {
        helpers::type_check_and_verify(R"(
            const to_opaque := fn(p: ^i32): ^opaque {
                return @ptrCast(^opaque, p);
            };
            const from_opaque := fn(p: ^mut opaque): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )");
    }

    SECTION("Casting away const via opaque pointer without constCast fails") {
        helpers::test_checker_fail(
            R"(
            const f := fn(p: ^opaque): ^mut i32 {
                return @ptrCast(^mut i32, p);
            };
        )",
            sema::diagnostic{"Cannot cast away const from pointer without @constCast",
                             sema::error::ILLEGAL_CONST_CAST,
                             std::pair{2UZ, 42UZ}});
    }
}

} // namespace ghoti::tests
