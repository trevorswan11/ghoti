#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view DARWIN{R"(
    pub using Handle = i32;

    pub const stdout: Handle = 12;
    pub const stderr: Handle = 30;

    pub const combine := fn(a: Handle, b: i32): i32 { return a + b; };
)"};

constexpr std::string_view STD{R"(
    pub import "darwin.gh" as darwin;
)"};

constexpr std::string_view SYS{R"(
    pub import "std.gh" as std;
)"};

} // namespace

TEST_CASE("E2E: a `const` folds through a chained re-export used as an operand") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "std.gh" as std;

            pub const main := fn(): i32 {
                return std::darwin::stdout + std::darwin::stderr;
            };
        )",
        {
            helpers::mock_file{"darwin.gh", DARWIN},
            helpers::mock_file{"std.gh", STD, "std"},
        })};

    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a `const` folds through a chained re-export passed as a call argument") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "std.gh" as std;

            pub const main := fn(): i32 {
                return std::darwin::combine(std::darwin::stdout, 30);
            };
        )",
        {
            helpers::mock_file{"darwin.gh", DARWIN},
            helpers::mock_file{"std.gh", STD, "std"},
        })};

    CHECK(exit_code == 42);
}

TEST_CASE("E2E: a `const` folds through a three-hop re-export chain") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "sys.gh" as sys;

            pub const main := fn(): i32 {
                return sys::std::darwin::combine(sys::std::darwin::stdout, 30);
            };
        )",
        {
            helpers::mock_file{"darwin.gh", DARWIN},
            helpers::mock_file{"std.gh", STD, "std"},
            helpers::mock_file{"sys.gh", SYS, "sys"},
        })};

    CHECK(exit_code == 42);
}

} // namespace ghoti::tests
