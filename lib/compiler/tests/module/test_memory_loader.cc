#include <string>

#include <catch2/catch_test_macros.hpp>

#include "compiler/module/error.hh"
#include "compiler/module/memory_loader.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("Correct add/load cycle") {
    const std::string  expected_content = "This is a file";
    mod::memory_loader loader;
    loader.add("mock", expected_content);

    const auto content{UNWRAP(loader.load("mock"))};
    CHECK(content == expected_content);
}

TEST_CASE("Overwriting entries in the VFS") {
    mod::memory_loader loader;
    loader.add("mock", "This is the bad content");

    const std::string expected_content = "This is the good content";
    loader.add("mock", expected_content);

    const auto content{UNWRAP(loader.load("mock"))};
    CHECK(content == expected_content);
}

TEST_CASE("Missing path in VFS") {
    mod::memory_loader loader;
    const auto         actual{UNWRAP_ERR(loader.load("mock"))};

    const mod::diagnostic expected{"Could not find path 'mock' in virtual file system",
                                   mod::error::PATH_DOES_NOT_EXIST};
    CHECK(actual == expected);
}

} // namespace ghoti::tests
