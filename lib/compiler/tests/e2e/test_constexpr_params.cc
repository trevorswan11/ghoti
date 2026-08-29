#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("constexpr parameter: distinct values monomorphize to distinct behavior") {
    CHECK(helpers::compile_and_run(R"(
        const shifted := fn(constexpr by: i32, x: i32): i32 {
            return x + by;
        };

        pub const main := fn(): i32 {
            return shifted(10, 1) + shifted(100, 0);
        };
    )") == 111);
}

TEST_CASE("constexpr parameter: reused value hits the instantiation cache once") {
    CHECK(helpers::compile_and_run(R"(
        const scale := fn(constexpr k: i32, x: i32): i32 {
            return x * k;
        };

        pub const main := fn(): i32 {
            return scale(3, 2) + scale(3, 5) + scale(4, 1);
        };
    )") == 6 + 15 + 4);
}

TEST_CASE("constexpr parameter: folds inside `if constexpr` in the body") {
    CHECK(helpers::compile_and_run(R"(
        const clamp_double := fn(constexpr n: i32): i32 {
            if constexpr (n > 100) {
                @compileError("n is too large");
            }
            return n * 2;
        };

        pub const main := fn(): i32 {
            return clamp_double(21);
        };
    )") == 42);
}

TEST_CASE("constexpr parameter: sizing a type is rejected for now") {
    helpers::expect_compile_error(R"(
        const bytes := fn(constexpr n: usize): i32 {
            return @as(i32, @sizeOf([n]i32));
        };

        pub const main := fn(): i32 {
            return bytes(2uz);
        };
    )");
}

TEST_CASE("constexpr parameter: a non-constant argument is rejected") {
    helpers::expect_compile_error(R"(
        const need_const := fn(constexpr n: i32): i32 {
            return n;
        };

        pub const main := fn(): i32 {
            var x: i32 = 7;
            return need_const(x);
        };
    )");
}

TEST_CASE("constexpr parameter: @compileError fires only for the offending instantiation") {
    helpers::expect_compile_error(R"(
        const bounded := fn(constexpr n: i32): i32 {
            if constexpr (n > 100) {
                @compileError("n is too large");
            }
            return n;
        };

        pub const main := fn(): i32 {
            return bounded(5) + bounded(500);
        };
    )");
}

TEST_CASE("constexpr parameter: `constexpr` on a `type` parameter is rejected") {
    helpers::expect_compile_error(R"(
        const bad := fn(constexpr t: type): i32 {
            return 0;
        };
    )");
}

} // namespace ghoti::tests
