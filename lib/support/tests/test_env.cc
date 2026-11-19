#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "support/env.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Environment variables get and set") {
    set_env("GHOTI_TEST_VAR", "hello_world");
    const auto& val{UNWRAP(get_env("GHOTI_TEST_VAR"))};
    CHECK(val == "hello_world");
}

TEST_CASE("Environment paths drop surrounding quotes and whitespace") {
    const std::string value{GENERATE("lib/std/std.gh",
                                     "\"lib/std/std.gh\"",
                                     "'lib/std/std.gh'",
                                     "  \"lib/std/std.gh\" ",
                                     "\tlib/std/std.gh\r\n")};
    set_env("GHOTI_TEST_PATH_VAR", value);

    // A relative path is resolved against the working directory
    const auto expected{(std::filesystem::current_path() / "lib/std/std.gh").lexically_normal()};
    CHECK(UNWRAP(get_env_path("GHOTI_TEST_PATH_VAR")) == expected);
}

TEST_CASE("Environment paths are none when unset or blank") {
    CHECK_FALSE(get_env_path("GHOTI_TEST_UNSET_PATH_VAR_3f9a"));
    const std::string value{GENERATE("\"\"", "   ", "''")};
    set_env("GHOTI_TEST_PATH_VAR", value);
    CHECK_FALSE(get_env_path("GHOTI_TEST_PATH_VAR"));
}

} // namespace ghoti::tests
