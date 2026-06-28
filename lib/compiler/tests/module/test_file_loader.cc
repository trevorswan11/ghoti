#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/module/error.hh"
#include "compiler/module/file_loader.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

constexpr std::string_view expected_contents{"This is ghoti-ful\n"};

TEST_CASE("Reading file off of disk correctly") {
    mod::file_loader loader;
    const auto normalized{helpers::unwrap(loader.normalize("lib/compiler/tests/module/mock.inc"))};

    const auto contents{helpers::unwrap(loader.load(normalized))};
    CHECK(contents.size() == expected_contents.size());
    CHECK(contents == expected_contents);
}

TEST_CASE("Path load on file not on disk") {
    mod::file_loader loader;
    const auto normalized{helpers::unwrap(loader.normalize("lib/compiler/tests/module/mock"))};
    const auto actual{helpers::unwrap_err(loader.load(normalized))};

    const mod::diagnostic expected{fmt::format("Path '{}' does not exist", normalized.string()),
                                   mod::error::PATH_DOES_NOT_EXIST};
    CHECK(actual == expected);
}

TEST_CASE("Path load on directory on disk") {
    mod::file_loader loader;
    const auto       normalized{helpers::unwrap(loader.normalize("lib/compiler/tests/module"))};
    const auto       actual{helpers::unwrap_err(loader.load(normalized))};

    const mod::diagnostic expected{fmt::format("Path '{}' is not a file", normalized.string()),
                                   mod::error::PATH_IS_NOT_FILE};
    CHECK(actual == expected);
}

} // namespace ghoti::tests
