#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view IMPORTED_TESTS{R"(
    test "imported failing" { @require(1 + 1 == 3); }
    test "imported passing" { @expect(true); }
)"};

constexpr std::string_view IMPORTED_FN{R"(
    pub const answer := fn(): i32 { return 42; };
)"};

} // namespace

TEST_CASE("E2E: a root test block that only imports a file runs that file's tests") {
    CHECK(helpers::compile_and_run_tests(
              R"(test "bundle" { import "suite.gh" as suite; })",
              {helpers::mock_file{"suite.gh", IMPORTED_TESTS, "suite"}}) != 0);
}

TEST_CASE("E2E: an import inside a function body still links the imported module") {
    CHECK(helpers::compile_and_run(
              R"(
            pub const main := fn(): i32 {
                import "dep.gh" as dep;
                return dep::answer();
            };
        )",
              {helpers::mock_file{"dep.gh", IMPORTED_FN, "dep"}}) == 42);
}

} // namespace ghoti::tests
