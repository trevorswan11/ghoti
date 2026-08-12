#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("nullptr initializes a raw pointer and compares equal to itself") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var p: ^i32 = nullptr;
            return if (p == nullptr) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("nullptr compares not-equal to a pointer to a real value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            return if (p != nullptr) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("nullptr can be reassigned into an existing pointer variable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            var p: ^i32 = ^x;
            p = nullptr;
            return if (p == nullptr) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("nullptr is rejected when used to initialize a reference") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            var r: &i32 = nullptr;
            return *r;
        };
    )");
}

TEST_CASE("nullptr is rejected when passed where a reference parameter is expected") {
    helpers::expect_compile_error(R"(
        const takes_ref := fn(r: &i32): i32 {
            return *r;
        };
        pub const main := fn(): i32 {
            return takes_ref(nullptr);
        };
    )");
}

} // namespace ghoti::tests
