#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a `constexpr fn`'s unfoldable early-return condition doesn't get skipped") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr len_eq := fn(a: []u8, b: []u8): bool {
            if (a.len != b.len) return false;
            return true;
        };

        test "differing-length slices are not len_eq" {
            @expect(!len_eq("abcdefg", "abcdef"));
        }

        test "same-length, same-content slices are len_eq" {
            @expect(len_eq("abcdef", "abcdef"));
        }
    )") == 0);
}

TEST_CASE("`eql`-shaped constexpr fn: early length mismatch and content mismatch both fold "
         "correctly") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr eql := fn(T: type, a: []T, b: []T): bool {
            if (a.len != b.len) return false;
            if (a.len == 0) return true;
            if (@typeInfo(T) != .float and a.ptr == b.ptr) return true;
            for (a, b) |a_elem, b_elem| {
                if (a_elem != b_elem) return false;
            }
            return true;
        };

        test "eql over bytes" {
            @expect(eql(u8, "abcd", "abcd"));
            @expect(!eql(u8, "abcdef", "abZdef"));
            @expect(!eql(u8, "abcdefg", "abcdef"));
        }
    )") == 0);
}

} // namespace ghoti::tests
