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

TEST_CASE("A range match arm dispatches on interval membership") {
    constexpr std::string_view program{R"(
        const classify := fn(n: i32): i32 {
            return match (n) {
                0..10 => 1,
                10..=20 => |v| v,
                _ => -1,
            };
        };
        pub const main := fn(): i32 {
            return classify(3) + classify(20) + classify(99);
        };
    )"};
    // 1 (0..10) + 20 (10..=20 capture) + -1 (catch-all) == 20
    CHECK(helpers::compile_and_run(program) == 20);
}

TEST_CASE("'match constexpr' selects its arm at compile time") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            return match constexpr (2) {
                1 => 10,
                2 => 20,
                _ => missing(),
            };
        };
    )") == 20);
}

TEST_CASE("'match constexpr' arm binds a capture to the folded scrutinee") {
    CHECK(helpers::compile_and_run(R"(
        constexpr LEVEL := 2;
        pub const main := fn(): i32 {
            return match constexpr (LEVEL) {
                0 => |v| v,
                1, 2, 3 => |v| v * 10,
                _ => -1,
            };
        };
    )") == 20);
}

TEST_CASE("'match constexpr' capture works per generic instantiation") {
    constexpr std::string_view program{R"(
        const scaled := fn(constexpr n: i32): i32 {
            return match constexpr (n) {
                1, 2 => |v| v * 10,
                _ => 0,
            };
        };
        pub const main := fn(): i32 {
            return scaled(2) + scaled(9);
        };
    )"};
    CHECK(helpers::compile_and_run(program) == 20);
}

TEST_CASE("'match constexpr' folds per generic instantiation") {
    constexpr std::string_view program{R"(
        const tag := fn(T: type): i32 {
            return match constexpr (T) {
                i32 => 4,
                i64 => 8,
                _ => 0,
            };
        };
        pub const main := fn(): i32 {
            return tag(i32) + tag(i64);
        };
    )"};
    CHECK(helpers::compile_and_run(program) == 12);
}

TEST_CASE("A multi-value match arm is taken when any listed pattern matches") {
    constexpr std::string_view program{R"(
        const kind := fn(n: i32): i32 {
            return match (n) {
                0, 2, 4..8 => |v| v + 1,
                1, 3 => 100,
                _ => -1,
            };
        };
        pub const main := fn(): i32 {
            return kind(2) + kind(3) + kind(6) + kind(9);
        };
    )"};
    // 3 (0,2,4..8 -> v+1) + 100 (1,3) + 7 (4..8 -> v+1) + -1 (catch-all) == 109
    CHECK(helpers::compile_and_run(program) == 109);
}

TEST_CASE("A range match arm accepts runtime endpoints") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var x: i32 = 7;
            var lo: i32 = 5;
            var hi: i32 = 9;
            return match (x) {
                lo..hi => 1,
                _ => 0,
            };
        };
    )") == 1);
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
