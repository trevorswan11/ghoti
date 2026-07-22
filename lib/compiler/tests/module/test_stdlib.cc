#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "compiler/module/stdlib.hh"
#include "support/env.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

TEST_CASE("Stdlib discovery via GHOTI_STDLIB env var") {
    tempfile tmp_std{"fake_std.gh"};
    {
        std::ofstream out{tmp_std.path};
        fmt::print(out, "// Fake stdlib");
    }

    set_env("GHOTI_STDLIB", tmp_std.path.string());
    const auto found{mod::find_stdlib()};
    REQUIRE(found.has_value());
    CHECK(*found == tmp_std.path);
}

TEST_CASE("Stdlib discovery via directory hierarchy search") {
    set_env("GHOTI_STDLIB", "/non_existent_stdlib_path/std.gh");
    const auto found{mod::find_stdlib()};
    REQUIRE(found.has_value());
    CHECK(std::filesystem::exists(*found));
    CHECK(found->filename() == "std.gh");
}

} // namespace ghoti::tests
