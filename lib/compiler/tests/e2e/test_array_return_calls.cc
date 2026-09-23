#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view DIGITS{R"(
    pub constexpr digits2 := fn(value: u8): [2]u8 {
        constexpr lookup := "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";
        const slice := lookup[value * 2..][0..2];
        return [2]u8{ slice[0], slice[1] };
    };

    constexpr N := 3;
    pub const triple := fn(v: u8): [N]u8 { return [N]u8{ v, v + 1, v + 2 }; };
)"};

constexpr std::string_view REEXPORT{R"(
    import "digits.gh" as digits;
    pub constexpr digits2 := digits.digits2;
    pub const triple := digits.triple;
)"};

} // namespace

TEST_CASE("E2E: a `[n]T`-returning call with a runtime argument is typed as an array") {
    const auto exit_code{helpers::compile_and_run(R"(
        const pair := fn(v: u8): [2]u8 { return [2]u8{ v, v + 1 }; };

        pub const main := fn(): i32 {
            var q: u8 = 4;
            q += 1;
            const d := pair(q);
            const r: i32 = d[0] + d[1];
            return r;
        };
    )")};

    CHECK(exit_code == 11);
}

TEST_CASE("E2E: a `[n]T`-returning function called through a local alias") {
    const auto exit_code{helpers::compile_and_run(R"(
        constexpr N := 2;
        const pair := fn(v: u8): [N]u8 { return [N]u8{ v, v + 1 }; };
        const alias := pair;

        pub const main := fn(): i32 {
            const r: i32 = alias(4)[1];
            return r;
        };
    )")};

    CHECK(exit_code == 5);
}

TEST_CASE("E2E: a `[n]T`-returning function called across a module boundary") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "digits.gh" as digits;

            pub const main := fn(): i32 {
                var v: u8 = 40;
                v += 2;
                const d := digits.digits2(v);
                const t: [3]u8 = digits.triple(v);
                const tens: i32 = d[0] - '0';
                const ones: i32 = d[1] - '0';
                const last: i32 = t[2];
                return tens * 10 + ones + last;
            };
        )",
        {helpers::mock_file{"digits.gh", DIGITS, "digits"}})};

    CHECK(exit_code == 42 + 44);
}

TEST_CASE("E2E: a `[n]T`-returning function re-exported through another module") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "reexport.gh" as re;

            pub const main := fn(): i32 {
                const d := re.digits2(17);
                const t := re.triple(1);
                const tens: i32 = d[0] - '0';
                const ones: i32 = d[1] - '0';
                const last: i32 = t[2];
                return tens * 10 + ones + last;
            };
        )",
        {
            helpers::mock_file{"digits.gh", DIGITS, "digits"},
            helpers::mock_file{"reexport.gh", REEXPORT, "reexport"},
        })};

    CHECK(exit_code == 17 + 3);
}

} // namespace ghoti::tests
