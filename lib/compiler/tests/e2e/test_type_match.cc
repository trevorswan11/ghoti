#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("match on a type alias dispatches to the matching arm") {
    CHECK(helpers::compile_and_run(R"(
        const T := i64;
        pub const main := fn(): i32 {
            return match (T) {
                i32  => 1,
                i64  => 2,
                bool => 3,
                _    => 9,
            };
        };
    )") == 2);
}

TEST_CASE("a non-selected type-match arm is not type-checked") {
    CHECK(helpers::compile_and_run(R"(
        const T := i32;
        pub const main := fn(): i32 {
            return match (T) {
                i32  => 10,
                bool => this_identifier_is_never_declared + neither_is_this,
                _    => 0,
            };
        };
    )") == 10);
}

TEST_CASE("type-match falls through to the catch-all when nothing matches") {
    CHECK(helpers::compile_and_run(R"(
        const T := f64;
        pub const main := fn(): i32 {
            return match (T) { i32 => 1, bool => 2, _ => 42 };
        };
    )") == 42);
}

TEST_CASE("a generic `T: type` selects a different arm per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const code := fn(T: type): i32 {
            return match (T) {
                i32  => 1,
                i64  => 2,
                bool => wrong_branch_should_be_pruned,
                _    => 0,
            };
        };
        pub const main := fn(): i32 {
            return code(i32) * 10 + code(i64);
        };
    )") == 12);
}

TEST_CASE("match on a pointer-type alias") {
    CHECK(helpers::compile_and_run(R"(
        const T := ^i32;
        pub const main := fn(): i32 {
            return match (T) { i32 => 1, ^i32 => 7, ^bool => 3, _ => 0 };
        };
    )") == 7);
}

TEST_CASE("match on a named struct type") {
    CHECK(helpers::compile_and_run(R"(
        const A := struct { x: i32 };
        const B := struct { y: i32 };
        pub const main := fn(): i32 {
            const T := B;
            return match (T) { A => 1, B => 8, _ => 0 };
        };
    )") == 8);
}

} // namespace ghoti::tests
