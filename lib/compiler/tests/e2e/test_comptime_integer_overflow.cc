#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("untyped integer constants fold exactly past 64 bits") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const product := 111334094107016374 * 242;
            if (product != 26942850773897962508) { return 1; }
            const quadrupled := 9223372036854775808 * 4;
            if (quadrupled != 36893488147419103232) { return 2; }
            const doubled := 9223372036854775807 * 2;
            if (doubled != 18446744073709551614) { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("unsigned and shifted compile-time results wrap to their type like runtime") {
    CHECK(helpers::compile_and_run(R"(
        const id := fn(x: u8): u8 { return x; };
        pub const main := fn(): i32 {
            const max: u8 = 255;
            const wrapped := max + 1;
            if (wrapped != 0) { return 1; }
            if (wrapped != id(max) +% 1) { return 2; }
            const three: u8 = 3;
            const shifted := three << 7;
            if (shifted != 128) { return 3; }
            const small: i8 = 127;
            if (small +% 1 != -128) { return 4; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("signed compile-time overflow is an error, as it is a runtime panic") {
    const auto overflow = [](std::string_view type, std::pair<usize, usize> at) {
        return sema::diagnostic{fmt::format("Signed integer overflow in compile-time constant "
                                            "expression: the result does not fit '{}'",
                                            type),
                                sema::error::COMPTIME_EVALUATION_FAILED,
                                at};
    };

    SECTION("addition") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    const a: i32 = 2147483647;
    const b := a + 1;
};
)",
                                   overflow("i32", {3, 15}));
    }
    SECTION("multiplication past 64 bits") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    const a: i64 = 9223372036854775807;
    const b := a * 2;
};
)",
                                   overflow("i64", {3, 15}));
    }
    SECTION("i128 itself") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    const a: i128 = 170141183460469231731687303715884105727;
    const b := a + 1;
};
)",
                                   overflow("i128", {3, 15}));
    }
    SECTION("the minimum divided by negative one") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    const a: i8 = -128;
    const b := a / -1;
};
)",
                                   overflow("i8", {3, 15}));
    }
    SECTION("negating the minimum") {
        helpers::test_checker_fail(R"(
const f := fn(): void {
    const m: i32 = -2147483648;
    const n := -m;
};
)",
                                   overflow("i32", {3, 15}));
    }
}

} // namespace ghoti::tests
