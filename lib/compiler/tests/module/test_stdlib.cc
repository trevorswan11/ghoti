#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>
#include <fmt/ostream.h>

#include "compiler/module/stdlib.hh"
#include "support/env.hh"
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
    CHECK(std::filesystem::exists(found));
    CHECK(found.filename() == "std.gh");
}

} // namespace ghoti::tests
