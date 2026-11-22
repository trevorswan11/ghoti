#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/module/file_loader.hh"
#include "compiler/module/stdlib.hh"
#include "helpers/uir_diff.hh"
#include "support/env.hh"

namespace ghoti::tests {

namespace {

// Every `.gh` file under the corpus directories, each analyzed as a root of its own
[[nodiscard]] auto corpus_files(const std::filesystem::path& repo)
    -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> files;
    for (const auto* dir : std::array{"examples", "lib/std", "lib/compiler_rt"}) {
        const auto root{repo / dir};
        if (!std::filesystem::exists(root)) { continue; }
        for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
            if (entry.is_regular_file() && entry.path().extension() == ".gh") {
                files.emplace_back(entry.path());
            }
        }
    }
    std::ranges::sort(files);
    return files;
}

} // namespace

TEST_CASE("UIR Sema's GIR matches the old pipeline's over the file corpus", "[.uir-diff]") {
    const auto repo{get_env("GHOTI_REPO_ROOT")};
    REQUIRE(repo);
    const auto stdlib{mod::find_stdlib()};
    REQUIRE(stdlib);
    const std::array libraries{std::pair{std::string{"std"}, *stdlib}};

    mod::file_loader             loader;
    std::map<std::string, usize> counts; // ordered for deterministic printing
    std::vector<std::string>     failures;
    // `GHOTI_UIR_CORPUS_FILTER=<text>` runs only the files whose path contains it
    const auto filter{get_env("GHOTI_UIR_CORPUS_FILTER")};
    for (const auto& file : corpus_files(std::filesystem::path{*repo})) {
        if (filter && !file.generic_string().contains(*filter)) { continue; }
        const auto diff{helpers::diff_pipelines(loader, file, libraries, {})};
        helpers::report_uir_diff(diff, file.generic_string());
        counts[std::string{helpers::uir_diff_status_name(diff.status)}] += 1;
        if (diff.status == helpers::uir_diff_status::GIR_DIFF ||
            diff.status == helpers::uir_diff_status::REJECTED) {
            failures.emplace_back(fmt::format("{}: {}\n{}",
                                              file.generic_string(),
                                              helpers::uir_diff_status_name(diff.status),
                                              diff.detail));
        }
    }
    for (const auto& [status, n] : counts) { fmt::println("{:>16}: {}", status, n); }
    for (const auto& failure : failures) { FAIL_CHECK(failure); }
}

} // namespace ghoti::tests
