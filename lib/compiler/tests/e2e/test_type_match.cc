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

TEST_CASE("match on slice and array type literals") {
    CHECK(helpers::compile_and_run(R"(
        const T := []u8;
        pub const main := fn(): i32 {
            return match (T) { u8 => 1, []u8 => 7, [4]u8 => 3, _ => 0 };
        };
    )") == 7);
    CHECK(helpers::compile_and_run(R"(
        const T := [4]bool;
        pub const main := fn(): i32 {
            return match (T) { []bool => 1, [4]bool => 8, [3]bool => 2, _ => 0 };
        };
    )") == 8);
}

TEST_CASE("a pointer/slice type passes through a `T: type` parameter") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { x: i32 };
        const tag := fn(T: type): i32 {
            return match (T) { ^Point => 8, Point => 4, i32 => 2, _ => 0 };
        };
        pub const main := fn(): i32 { return tag(^Point) * 10 + tag(Point); };
    )") == 84);
    CHECK(helpers::compile_and_run(R"(
        const kind := fn(T: type): i32 {
            return match (T) { []u8 => 10, [8]u8 => 20, i32 => 30, _ => 0 };
        };
        pub const main := fn(): i32 {
            const A := []u8;
            const B := [8]u8;
            return kind(A) + kind(B) + kind(i32);
        };
    )") == 60);
}

TEST_CASE("match on a function-type literal") {
    CHECK(helpers::compile_and_run(R"(
        const T := fn(i32): void;
        pub const main := fn(): i32 {
            return match (T) {
                fn(i32): i32  => 1,
                fn(i32): void => 8,
                fn(): void    => 2,
                _             => 0,
            };
        };
    )") == 8);
}

TEST_CASE("a function-type match arm works across multiple generic instantiations") {
    CHECK(helpers::compile_and_run(R"(
        const kind := fn(T: type): i32 {
            return match (T) {
                fn(i32): void => 1,
                _             => 0,
            };
        };
        pub const main := fn(): i32 {
            const F := fn(i32): void;
            return kind(F) * 100 + kind(i32) * 10 + kind(F);
        };
    )") == 101);
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
