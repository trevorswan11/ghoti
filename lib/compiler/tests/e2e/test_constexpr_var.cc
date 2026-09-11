#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`constexpr var` reads back its initializer") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 5;
            return n;
        };
    )") == 5);
}

TEST_CASE("`constexpr var` can be reassigned with `=`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 5;
            n = n + 3;
            return n;
        };
    )") == 8);
}

TEST_CASE("`constexpr var` can be reassigned with a compound op") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            n += 3;
            n += 4;
            return n;
        };
    )") == 7);
}

TEST_CASE("`constexpr var` reassignment in a nested block persists to the outer scope") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr var n := 0;
            {
                n = n + 5;
            }
            n = n + 2;
            return n;
        };
    )") == 7);
}

TEST_CASE("a `constexpr var` initializer must fold") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            constexpr var n := x;
            return n;
        };
    )");
}

TEST_CASE("a `constexpr var` assignment's RHS must fold") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            constexpr var n := 0;
            n = x;
            return n;
        };
    )");
}

TEST_CASE("`constexpr var` does not yet support aggregate types") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            constexpr var arr: [2]i32 = .{1, 2};
            return arr[0];
        };
    )");
}

} // namespace ghoti::tests
