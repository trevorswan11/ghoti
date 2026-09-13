#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`++` concatenates two constexpr byte-strings") {
    helpers::resolve_and_check(R"(constexpr s := "ab" ++ "cde";)");
}

TEST_CASE("`++` concatenates two constexpr arrays of the same element type") {
    helpers::resolve_and_check(R"(
        const a: [2]i32 = .{1, 2};
        const b: [3]i32 = .{3, 4, 5};
        constexpr c := a ++ b;
    )");
}

TEST_CASE("`++` on non-array/slice operands is a compile error") {
    CHECK(helpers::raised("constexpr x := 1 ++ 2;", sema::error::CONCAT_REQUIRES_ARRAY_OR_SLICE));
}

TEST_CASE("`++` on mismatched element types is a compile error") {
    CHECK(helpers::raised(R"(
        const a: [2]i32 = .{1, 2};
        constexpr x := "ab" ++ a;
    )",
                          sema::error::CONCAT_ELEM_TYPE_MISMATCH));
}

TEST_CASE("`++` on a non-foldable operand is a compile error") {
    CHECK(helpers::raised(R"(
        const use := fn(s: []u8): void {
            const x := s ++ "y";
        };
    )",
                          sema::error::CONCAT_NOT_FOLDABLE));
}

} // namespace ghoti::tests
