#include <filesystem>
#include <string>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "compiler/module/file_loader.hh"
#include "compiler/module/stdlib.hh"
#include "helpers/common.hh"
#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

// The shipped std, diffed with `std` registered as the driver registers it
[[nodiscard]] auto diff_std(const std::filesystem::path&      root,
                            const helpers::pipeline_settings& settings) -> helpers::uir_diff {
    mod::file_loader                                    loader;
    const std::pair<std::string, std::filesystem::path> std_lib{"std", UNWRAP(mod::find_stdlib())};
    auto diff{helpers::diff_pipelines(loader, root, {&std_lib, 1}, settings)};
    helpers::report_uir_diff(diff);
    return diff;
}

// The same GIR, or the same effects with values computed in another place
[[nodiscard]] auto agrees(const helpers::uir_diff& diff) -> bool {
    return diff.status == helpers::uir_diff_status::MATCH ||
           diff.status == helpers::uir_diff_status::EVAL_ORDER ||
           diff.status == helpers::uir_diff_status::FOLD_PLACEMENT;
}

} // namespace

TEST_CASE("std's tests analyze alike in both pipelines, with the entry code", "[uir]") {
    // TODO: std's SIMD tests need runtime vectors, which phase 8e analyzes
    const auto diff{diff_std(UNWRAP(mod::find_stdlib()), {.testing = true, .entry = true})};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK((agrees(diff) || diff.status == helpers::uir_diff_status::UNSUPPORTED));
}

TEST_CASE("std analyzes alike in both pipelines as a library", "[uir]") {
    const auto diff{diff_std(UNWRAP(mod::find_stdlib()), {})};
    INFO(helpers::uir_diff_status_name(diff.status));
    INFO(diff.detail);
    CHECK(agrees(diff));
}

} // namespace ghoti::tests
