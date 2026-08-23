#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/module/error.hh"
#include "compiler/module/overlay_loader.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

constexpr std::string_view disk_contents{"This is ghoti-ful\n"};
constexpr std::string_view mock_path{"lib/compiler/tests/module/mock.inc"};

TEST_CASE("Overlay-less load falls back to disk") {
    mod::overlay_loader loader;
    const auto          normalized{UNWRAP(loader.normalize(mock_path))};
    CHECK(UNWRAP(loader.load(normalized)) == disk_contents);
}

TEST_CASE("Overlay takes priority over disk contents") {
    mod::overlay_loader loader;
    const std::string   expected_content{"overlaid content"};
    CHECK(loader.add(mock_path, expected_content));
    CHECK(UNWRAP(loader.load(mock_path)) == expected_content);
}

TEST_CASE("Overwriting an existing overlay") {
    mod::overlay_loader loader;
    CHECK(loader.add(mock_path, "stale content"));

    const std::string expected_content{"fresh content"};
    CHECK(loader.add(mock_path, expected_content));
    CHECK(UNWRAP(loader.load(mock_path)) == expected_content);
}

TEST_CASE("Removing an overlay reverts to disk contents") {
    mod::overlay_loader loader;
    CHECK(loader.add(mock_path, "overlaid content"));
    loader.remove(mock_path);
    CHECK(UNWRAP(loader.load(mock_path)) == disk_contents);
}

TEST_CASE("Missing path with no overlay and nothing on disk") {
    mod::overlay_loader loader;
    const auto          normalized{UNWRAP(loader.normalize("lib/compiler/tests/module/mock"))};
    const auto          actual{UNWRAP_ERR(loader.load(normalized))};

    const mod::diagnostic expected{fmt::format("Path '{}' does not exist", normalized.string()),
                                   mod::error::PATH_DOES_NOT_EXIST};
    CHECK(actual == expected);
}

TEST_CASE("Path load on directory on disk with no overlay") {
    mod::overlay_loader loader;
    const auto          normalized{UNWRAP(loader.normalize("lib/compiler/tests/module"))};
    const auto          actual{UNWRAP_ERR(loader.load(normalized))};

    const mod::diagnostic expected{fmt::format("Path '{}' is not a file", normalized.string()),
                                   mod::error::PATH_IS_NOT_FILE};
    CHECK(actual == expected);
}

} // namespace ghoti::tests
