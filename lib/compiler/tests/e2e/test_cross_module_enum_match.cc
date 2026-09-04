#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view MOD_A{R"(
    pub const MyErr := enum { A = 1, B = 2, _ };
    pub const make := fn(v: i32): MyErr { return @as(MyErr, v); };
)"};

} // namespace

TEST_CASE("E2E: match on a cross-module enum honors explicit discriminant values") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "a.gh" as a;
            pub const main := fn(): i32 {
                const e: a::MyErr = a::make(2);
                return match (e) { .A => 1, .B => 2, _ => 99 };
            };
        )",
        {helpers::mock_file{"a.gh", MOD_A, "a"}})};
    CHECK(exit_code == 2);
}

TEST_CASE("E2E: `==` against a cross-module enum's `Type.Variant` honors explicit values") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "a.gh" as a;
            pub const main := fn(): i32 {
                const e: a::MyErr = a::make(1);
                if (e == a::MyErr.A) { return 7; }
                return 0;
            };
        )",
        {helpers::mock_file{"a.gh", MOD_A, "a"}})};
    CHECK(exit_code == 7);
}

TEST_CASE("E2E: an exhaustive cross-module enum match doesn't trap on a legitimately-matched arm") {
    constexpr std::string_view mod{R"(
        pub const Status := enum { ok = 10, fail = 20 };
        pub const make := fn(v: i32): Status { return @as(Status, v); };
    )"};
    const auto                 exit_code{helpers::compile_and_run(
        R"(
            import "s.gh" as s;
            pub const main := fn(): i32 {
                const st: s::Status = s::make(20);
                return match (st) {
                    .ok => 1,
                    .fail => 2,
                };
            };
        )",
        {helpers::mock_file{"s.gh", mod, "s"}})};
    CHECK(exit_code == 2);
}

TEST_CASE("E2E: `@tagName` of a cross-module enum with explicit values reports the right variant") {
    constexpr std::string_view mod{R"(
        pub const Color := enum { red = 5, green = 6, blue = 7 };
    )"};
    const auto                 exit_code{helpers::compile_and_run(
        R"(
            import "c.gh" as c;
            pub const main := fn(): i32 {
                const v: c::Color = .green;
                const s := @tagName(v);
                return @as(i32, s.len) * 10 + @as(i32, s[0]);
            };
        )",
        {helpers::mock_file{"c.gh", mod, "c"}})};
    CHECK(exit_code == (5 * 10 + 'g'));
}

} // namespace ghoti::tests
