#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("`@tagName` folds at compile time for a constexpr enum value") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        pub const main := fn(): i32 {
            const s := @tagName(Color.green);
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (5 + 'g'));
}

TEST_CASE("`@tagName` dispatches at runtime for a non-constant enum value") {
    CHECK(helpers::compile_and_run(R"(
        const Color := enum { red, green, blue };
        const name_of := fn(c: Color): []u8 { return @tagName(c); };
        pub const main := fn(): i32 {
            const s := name_of(.blue);
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + 'b'));
}

TEST_CASE("`@tagName` runtime dispatch honors explicit, non-positional discriminants") {
    CHECK(helpers::compile_and_run(R"(
        const Level := enum { low = 10, mid = 20, high = 30 };
        const to_level := fn(v: i32): Level { return @as(Level, v); };
        const name_of := fn(l: Level): []u8 { return @tagName(l); };
        pub const main := fn(): i32 {
            const s := name_of(to_level(30));
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (4 + 'h'));
}

TEST_CASE(
    "`@tagName` of a non-exhaustive enum falls back to \"_\" for a value with no listed variant") {
    CHECK(helpers::compile_and_run(R"(
        const Status := enum { ok = 1, fail = 2, _ };
        const make := fn(v: i32): Status { return @as(Status, v); };
        pub const main := fn(): i32 {
            const s := @tagName(make(99));   // 99 matches no listed variant
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (1 + '_'));
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
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (5 + 'c'));
}

TEST_CASE("`@tagName` of a tagged union tracks a field reassigned through a direct write") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { a: i32, b: i32 };
        pub const main := fn(): i32 {
            var u := U{ .a = 1 };
            u.b = 2;   // re-tags u as active field 'b'
            const s := @tagName(u);
            return @as(i32, s.len) + @as(i32, s[0]);
        };
    )") == (1 + 'b'));
}

TEST_CASE("`@tagName` folds for a raw-identifier enum variant") {
    CHECK(helpers::compile_and_run(R"(
        const Kw := enum { @"struct", @"fn", plain };
        pub const main := fn(): i32 {
            const s := @tagName(Kw.@"fn");
            return @as(i32, s.len) + @as(i32, s[0]);   // "fn"
        };
    )") == (2 + 'f'));
}

TEST_CASE("`@tagName` dispatches at runtime for a raw-identifier enum variant") {
    CHECK(helpers::compile_and_run(R"(
        const Kw := enum { @"struct", @"fn", plain };
        const name_of := fn(k: Kw): []u8 { return @tagName(k); };
        pub const main := fn(): i32 {
            const s := name_of(.@"struct");
            return @as(i32, s.len) + @as(i32, s[0]);   // "struct"
        };
    )") == (6 + 's'));
}

TEST_CASE("`@tagName` of a tagged union reports a raw-identifier field name") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { @"union": i32, @"match": i32 };
        const active := fn(u: U): []u8 { return @tagName(u); };
        pub const main := fn(): i32 {
            var u := U{ .@"match" = 7 };
            const s := active(u);
            return @as(i32, s.len) + @as(i32, s[0]);   // "match"
        };
    )") == (5 + 'm'));
}

TEST_CASE("`@tagName` folds for a raw-identifier tagged-union field") {
    CHECK(helpers::compile_and_run(R"(
        const U := union { @"union": i32, @"match": i32 };
        pub const main := fn(): i32 {
            const u := U{ .@"union" = 3 };
            const s := @tagName(u);
            return @as(i32, s.len) + @as(i32, s[0]);   // "union"
        };
    )") == (5 + 'u'));
}

} // namespace ghoti::tests
