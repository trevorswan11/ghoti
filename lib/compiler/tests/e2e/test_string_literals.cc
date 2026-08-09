#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Local string literal indexed directly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hi";
            return @as(i32, s[0]);
        };
    )") == 'h');
}

TEST_CASE("Local string literal .len includes the implicit null terminator") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hello";
            return @as(i32, s.len);
        };
    )") == 6);
}

TEST_CASE("Local string literal .ptr decays to a valid pointer to its bytes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hi";
            const p: ^u8 = s.ptr;
            return @as(i32, *p);
        };
    )") == 'h');
}

TEST_CASE("String literal passed directly to a slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const first_byte := fn(msg: []u8): i32 {
            return @as(i32, msg[0]);
        };
        pub const main := fn(): i32 {
            return first_byte("Ax");
        };
    )") == 'A');
}

TEST_CASE("String literal passed directly to a slice parameter preserves length") {
    CHECK(helpers::compile_and_run(R"(
        const len_of := fn(msg: []u8): i32 {
            return @as(i32, msg.len);
        };
        pub const main := fn(): i32 {
            return len_of("hello");
        };
    )") == 6);
}

TEST_CASE("Top-level scalar const referenced from a function") {
    CHECK(helpers::compile_and_run(R"(
        const FOO := 42;
        pub const main := fn(): i32 {
            return FOO;
        };
    )") == 42);
}

TEST_CASE("Top-level scalar const used inside an expression") {
    CHECK(helpers::compile_and_run(R"(
        const FOO := 42;
        pub const main := fn(): i32 {
            return FOO + 1;
        };
    )") == 43);
}

TEST_CASE("Top-level string const indexed directly (no .ptr)") {
    CHECK(helpers::compile_and_run(R"(
        const GREETING := "hi";
        pub const main := fn(): i32 {
            return @as(i32, GREETING[0]);
        };
    )") == 'h');
}

TEST_CASE("Top-level string const .ptr decays to a valid pointer to its bytes") {
    CHECK(helpers::compile_and_run(R"(
        const GREETING := "hi";
        pub const main := fn(): i32 {
            const p: ^u8 = GREETING.ptr;
            return @as(i32, *p);
        };
    )") == 'h');
}

} // namespace ghoti::tests
