#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/module/error.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "ghoti/config.h"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("Fetching non-relative file modules") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};

#if GHOTI_WINDOWS
    const std::string_view file{"C:/fake/foo.gh"};
#else
    const std::string_view file{"/fake/foo.gh"};
#endif
    const auto actual{UNWRAP_ERR(manager.try_get_file_module(file))};

    const mod::diagnostic expected{
        fmt::format("Requested file '{}' is absolute", file),
        mod::error::MODULE_PATH_NOT_RELATIVE,
    };
    CHECK(actual == expected);
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
