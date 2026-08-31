#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view MOD_A{R"(
    pub const VALUE: i32 = 100;
    pub const read_a := fn(): i32 { return VALUE; };
)"};

constexpr std::string_view MOD_B{R"(
    pub const OTHER: i32 = 55;
    pub const read_b := fn(): i32 { return OTHER; };
)"};

} // namespace

TEST_CASE("E2E: a `pub const` folds to its own module's value across imports") {
    const auto exit_code{helpers::compile_and_run(
        R"(
            import "a.gh" as a;
            import "b.gh" as b;

            pub const main := fn(): i32 {
                return a::read_a() + b::read_b();
            };
        )",
        {
            helpers::mock_file{"a.gh", MOD_A, "a"},
            helpers::mock_file{"b.gh", MOD_B, "b"},
        })};

    CHECK(exit_code == 155);
}

} // namespace ghoti::tests
