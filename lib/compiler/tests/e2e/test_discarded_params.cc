#include <catch2/catch_test_macros.hpp>

#include "compiler/syntax/error.hh"
#include "helpers/codegen.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("calling a function with a single discarded parameter") {
    CHECK(helpers::compile_and_run(R"(
        const ignore := fn(_: i32): i32 {
            return 42;
        };

        pub const main := fn(): i32 {
            return ignore(999);
        };
    )") == 42);
}

TEST_CASE("calling a function with multiple discarded parameters") {
    CHECK(helpers::compile_and_run(R"(
        const add_outer := fn(a: i32, _: bool, _: []u8, b: i32): i32 {
            return a + b;
        };

        pub const main := fn(): i32 {
            return add_outer(20, true, "ignored", 22);
        };
    )") == 42);
}

TEST_CASE("calling a function with a void discarded parameter") {
    CHECK(helpers::compile_and_run(R"(
        const do_nothing := fn(_: void): i32 {
            return 42;
        };

        pub const main := fn(): i32 {
            return do_nothing({});
        };
    )") == 42);
}

TEST_CASE("all parameters discarded") {
    CHECK(helpers::compile_and_run(R"(
        const const_answer := fn(_: i32, _: i32): i32 {
            return 42;
        };

        pub const main := fn(): i32 {
            return const_answer(1, 2);
        };
    )") == 42);
}

TEST_CASE("anonymous closure with discarded parameter") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const f := fn(_: i32): i32 { return 42; };
            return f(123);
        };
    )") == 42);
}

TEST_CASE("constexpr function with discarded parameter") {
    CHECK(helpers::compile_and_run(R"(
        constexpr get_val := fn(_: i32): i32 {
            return 42;
        };

        pub const main := fn(): i32 {
            const val := get_val(100);
            return val;
        };
    )") == 42);
}

TEST_CASE("generic function with discarded parameter") {
    CHECK(helpers::compile_and_run(R"(
        const identity_or_42 := fn(T: type, _: T): i32 {
            return 42;
        };

        pub const main := fn(): i32 {
            return identity_or_42(i32, 10);
        };
    )") == 42);
}

} // namespace ghoti::tests
