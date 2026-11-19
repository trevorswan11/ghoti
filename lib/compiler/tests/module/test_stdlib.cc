#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/util>

#include "compiler/module/stdlib.hh"
#include "support/env.hh"
#include "support/path_utils.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("Stdlib discovery via GHOTI_STDLIB env var") {
    tempfile tmp_std{"fake_std.gh"};
    {
        std::ofstream out{tmp_std.path};
        fmt::print(out, "// Fake stdlib");
    }

    set_env("GHOTI_STDLIB", tmp_std.path.string());
    CHECK(UNWRAP(mod::find_stdlib()) == tmp_std);
}

TEST_CASE("Stdlib discovery via directory hierarchy search") {
    set_env("GHOTI_STDLIB", "/non_existent_stdlib_path/std.gh");
    const auto found{UNWRAP(mod::find_stdlib())};
    CHECK(path_utils::exists(found));
    CHECK(found.filename() == "std.gh");
}

TEST_CASE("Shipped lib resources resolve next to the stdlib") {
    const auto darwin{UNWRAP(mod::find_lib_path("darwin"))};
    CHECK(path_utils::exists(darwin / "libSystem.tbd"));
    CHECK(path_utils::exists(darwin / "SDKSettings.json"));
    CHECK_FALSE(mod::find_lib_path("no_such_resource_7c1d"));
}

TEST_CASE("Quoted GHOTI_STDLIB overrides the shipped lib for std and compiler_rt") {
    // Issue #420: cmd.exe's `set GHOTI_STDLIB="..."` keeps the quotes in the value
    const tempdir checkout{"stdlib_quoted"};
    checkout.write("lib/std/std.gh", "// Checkout stdlib");
    checkout.write("lib/compiler_rt/compiler_rt.gh", "// Checkout compiler_rt");
    const auto std_path{checkout.path / "lib" / "std" / "std.gh"};

    const std::string previous{get_env("GHOTI_STDLIB").value_or("")};
    const auto        restore{gsl::finally([&] { set_env("GHOTI_STDLIB", previous); })};
    set_env("GHOTI_STDLIB", fmt::format("\"{}\"", std_path.string()));

    CHECK(std::filesystem::equivalent(UNWRAP(mod::find_stdlib()), std_path));
    CHECK(std::filesystem::equivalent(UNWRAP(mod::find_lib_path("compiler_rt/compiler_rt.gh")),
                                      checkout.path / "lib" / "compiler_rt" / "compiler_rt.gh"));
}

} // namespace ghoti::tests
