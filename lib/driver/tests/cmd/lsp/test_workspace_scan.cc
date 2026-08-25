#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "driver/cmd/lsp/workspace_scan.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

namespace {

auto contains(const std::vector<std::filesystem::path>& files, const std::filesystem::path& p)
    -> bool {
    return std::ranges::contains(files, std::filesystem::weakly_canonical(p));
}

} // namespace

TEST_CASE("discover_workspace_files finds .gh files recursively") {
    tempdir dir{"scan_basic"};
    dir.write("a.gh", "pub const a := 1;\n");
    dir.write("nested/b.gh", "pub const b := 1;\n");
    dir.write("not_ghoti.txt", "irrelevant\n");

    const auto files{lsp::discover_workspace_files({dir.path})};
    CHECK(contains(files, dir.path / "a.gh"));
    CHECK(contains(files, dir.path / "nested" / "b.gh"));
    CHECK(files.size() == 2);
}

TEST_CASE("discover_workspace_files is case-insensitive on the extension") {
    tempdir dir{"scan_case"};
    dir.write("upper.GH", "pub const a := 1;\n");

    const auto files{lsp::discover_workspace_files({dir.path})};
    CHECK(contains(files, dir.path / "upper.GH"));
}

TEST_CASE("discover_workspace_files skips excluded directories") {
    tempdir dir{"scan_excluded"};
    dir.write("real.gh", "pub const a := 1;\n");
    dir.write(".git/HEAD.gh", "pub const fake := 1;\n");
    dir.write(".zig-cache/stale.gh", "pub const fake := 1;\n");
    dir.write("zig-out/stale.gh", "pub const fake := 1;\n");
    dir.write("zig-pkg/stale.gh", "pub const fake := 1;\n");

    const auto files{lsp::discover_workspace_files({dir.path})};
    CHECK(contains(files, dir.path / "real.gh"));
    CHECK(files.size() == 1);
}

TEST_CASE("discover_workspace_files returns nothing for a root with no .gh files") {
    tempdir dir{"scan_empty"};
    dir.write("readme.md", "nothing to see here\n");

    CHECK(lsp::discover_workspace_files({dir.path}).empty());
}

} // namespace ghoti::tests
