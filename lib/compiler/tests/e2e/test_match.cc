#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Match over a tagged union dispatches to the active field") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            var u := U{ .b = 7 };
            return match (u) {
                .a => 1,
                .b => 2,
            };
        };
    )") == 2);
}

TEST_CASE("Match arm capture mutates the matched union field in place") {
    SECTION("Plain capture") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32 };
            pub const main := fn(): i32 {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |v| { v = v + 10; },
                    _ => {},
                };
                return u.a;
            };
        )") == 15);
    }

    SECTION("By mutable reference") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32 };
            pub const main := fn(): i32 {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |&mut v| { v = v + 10; },
                    _ => {},
                };
                return u.a;
            };
        )") == 15);
    }

    SECTION("By mutable pointer") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32 };
            pub const main := fn(): i32 {
                var u := U{ .a = 5 };
                match (u) {
                    .a => |^mut v| { *v = *v + 10; },
                    _ => {},
                };
                return u.a;
            };
        )") == 15);
    }
}

TEST_CASE("Match arm capture mutates a whole-value (non-union) in place") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            match (x) {
                5 => |v| { v = v + 10; },
                _ => {},
            };
            return x;
        };
    )") == 15);
}

TEST_CASE("A direct `union == .field` comparison checks the active field") {
    SECTION("True when the field is active") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32, b: i32 };
            pub const main := fn(): i32 {
                var u := U{ .b = 3 };
                if (u == .b) { return 42; }
                return 0;
            };
        )") == 42);
    }

    SECTION("False when a different field is active") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32, b: i32 };
            pub const main := fn(): i32 {
                var u := U{ .b = 3 };
                if (u == .a) { return 42; }
                return 0;
            };
        )") == 0);
    }

    SECTION("!= negates the comparison") {
        CHECK(helpers::compile_and_run(R"(
            const U := union { a: i32, b: i32 };
            pub const main := fn(): i32 {
                var u := U{ .b = 3 };
                if (u != .a) { return 42; }
                return 0;
            };
        )") == 42);
    }
}

} // namespace ghoti::tests
