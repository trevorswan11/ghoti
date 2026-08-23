#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

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

TEST_CASE("A plain match arm capture reads the current field value") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32 };
        pub const main := fn(): i32 {
            const u := U{ .a = 5 };
            return match (u) {
                .a => |v| v + 10,
            };
        };
    )") == 15);
}

TEST_CASE("Match arm capture mutates the matched union field in place") {
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

TEST_CASE("Match arm capture mutates a struct field matcher (dot_expr) in place") {
    CHECK(helpers::compile_and_run(R"(
        const Box := struct { val: i32 };
        pub const main := fn(): i32 {
            var b := Box{ .val = 5 };
            match (b.val) {
                5 => |&mut v| { v = v + 10; },
                _ => {},
            };
            return b.val;
        };
    )") == 15);
}

TEST_CASE("Match arm capture mutates an array element matcher (index_expr) in place") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var arr := [3]i32{1, 2, 3};
            match (arr[1]) {
                2 => |&mut v| { v = v + 10; },
                _ => {},
            };
            return arr[1];
        };
    )") == 12);
}

TEST_CASE("A non-value capture over an rvalue match matcher is rejected") {
    helpers::expect_compile_error(R"(
        const get_five := fn(): i32 {
            return 5;
        };

        pub const main := fn(): i32 {
            match (get_five()) {
                5 => |&mut v| { v = v + 10; },
                _ => {},
            };
            return 0;
        };
    )");
}

TEST_CASE("A non-value capture over an rvalue for-loop iterable is rejected") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            for (0..10) |&mut v| {
                v = v + 1;
            };
            return 0;
        };
    )");
}

TEST_CASE("Match arm capture mutates a whole-value (non-union) scrutinee by mutable reference") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 5;
            match (x) {
                5 => |&mut v| { v = v + 10; },
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
