#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`@tagName` folds at compile time for a constexpr enum value") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        pub const main := fn(): i32 {
            const s := @tagName(Color.green);
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (5 * 10 + 'g'));
}

TEST_CASE("`@tagName` dispatches at runtime for a non-constant enum value") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        const name_of := fn(c: Color): []u8 { return @tagName(c); };
        pub const main := fn(): i32 {
            const s := name_of(.blue);
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (4 * 10 + 'b'));
}

TEST_CASE("`@tagName` runtime dispatch honors explicit, non-positional discriminants") {
    CHECK(helpers::compile_and_run(R"(
        const Level := enum { low = 10, mid = 20, high = 30 };
        const to_level := fn(v: i32): Level { return @as(Level, v); };
        const name_of := fn(l: Level): []u8 { return @tagName(l); };
        pub const main := fn(): i32 {
            const s := name_of(to_level(30));
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (4 * 10 + 'h'));
}

TEST_CASE(
    "`@tagName` of a non-exhaustive enum falls back to \"_\" for a value with no listed variant") {
    CHECK(helpers::compile_and_run(R"(
        const Status := enum { ok = 1, fail = 2, _ };
        const make := fn(v: i32): Status { return @as(Status, v); };
        pub const main := fn(): i32 {
            const s := @tagName(make(99));   // 99 matches no listed variant
            return @as(i32, s.len) * 100 + @as(i32, s[0]);
        };
    )") == (1 * 100 + '_'));
}

TEST_CASE("`@tagName` of a non-exhaustive enum reports a variant's real name when it matches") {
    CHECK(helpers::compile_and_run(R"(
        const Status := enum { ok = 1, fail = 2, _ };
        const make := fn(v: i32): Status { return @as(Status, v); };
        pub const main := fn(): i32 {
            const s := @tagName(make(2));
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + 'f'));
}

TEST_CASE("`@tagName` of a tagged union reports the currently-active field's name") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, count: i32 };
        const active_name := fn(u: U): []u8 { return @tagName(u); };
        pub const main := fn(): i32 {
            var u := U{ .count = 5 };
            const s := active_name(u);
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (5 * 10 + 'c'));
}

TEST_CASE("`@tagName` of a tagged union tracks a field reassigned through a direct write") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            var u := U{ .a = 1 };
            u.b = 2;   // re-tags u as active field 'b'
            const s := @tagName(u);
            return @as(i32, s.len) * 10 + @as(i32, s[0]);
        };
    )") == (1 * 10 + 'b'));
}

} // namespace ghoti::tests
