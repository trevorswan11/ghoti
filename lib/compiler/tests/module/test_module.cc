#include <filesystem>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/module/error.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "ghoti/config.h"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("Fetching absolute file modules") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};

#if GHOTI_WINDOWS
    const std::filesystem::path root{"C:/fake"};
    const std::filesystem::path elsewhere{"D:/other"};
#else
    const std::filesystem::path root{"/fake"};
    const std::filesystem::path elsewhere{"/other"};
#endif
    loader.add(root / "lib" / "foo.gh", "pub const x := 1;");

    // An absolute import ignores its importer's directory and dedupes with the relative spelling
    const auto absolute{UNWRAP(manager.try_get_file_module(root / "lib" / "foo.gh", elsewhere))};
    const auto relative{UNWRAP(manager.try_get_file_module("lib/foo.gh", root))};
    CHECK(absolute.get() == relative.get());

    const auto missing{UNWRAP_ERR(manager.try_get_file_module(root / "missing.gh", elsewhere))};
    CHECK(missing.get_error() == mod::error::PATH_DOES_NOT_EXIST);
}

TEST_CASE("Fetching missing library modules") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    const auto          actual{UNWRAP_ERR(manager.try_get_library_module("foo"))};

    const mod::diagnostic expected{
        "Unknown module 'foo'",
        mod::error::MODULE_DOES_NOT_EXIST,
    };
    CHECK(actual == expected);
}

TEST_CASE("Adding duplicate library module") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    REQUIRE(manager.add_library_module("foo", "foo.gh"));
    const auto actual{UNWRAP_ERR(manager.add_library_module("foo", "src/foo.gh"))};

    const mod::diagnostic expected{
        "Attempt to add duplicate module 'foo' from path 'src/foo.gh' which already exists at "
        "path 'foo.gh'",
        mod::error::MODULE_ALREADY_EXISTS,
    };
    CHECK(actual == expected);
}

} // namespace ghoti::tests
