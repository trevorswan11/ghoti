#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ghoti/config.h"
#include "support/path_utils.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("uri_to_path rejects non-file schemes") {
    CHECK_FALSE(path_utils::uri_to_path("https://example.com/foo.gh").has_value());
    CHECK_FALSE(path_utils::uri_to_path("untitled:Untitled-1").has_value());
}

TEST_CASE("path_to_uri and uri_to_path round-trip a path with a space") {
    const std::filesystem::path path{std::filesystem::current_path() / "a dir" / "file.gh"};
    const auto                  uri{path_utils::path_to_uri(path)};
    CHECK(uri.find("%20") != std::string::npos);

    const auto roundtripped{UNWRAP(path_utils::uri_to_path(uri))};
    CHECK(roundtripped == path);
}

#if GHOTI_WINDOWS
TEST_CASE("uri_to_path strips the leading slash before a Windows drive letter") {
    const auto path{UNWRAP(path_utils::uri_to_path("file:///C:/Users/me/project/main.gh"))};
    CHECK(path == std::filesystem::path{"C:/Users/me/project/main.gh"});
}

TEST_CASE("uri_to_path decodes an encoded drive-letter colon") {
    const auto path{UNWRAP(path_utils::uri_to_path("file:///C%3A/Users/me/project/main.gh"))};
    CHECK(path == std::filesystem::path{"C:/Users/me/project/main.gh"});
}

TEST_CASE("path_to_uri leaves the drive-letter colon unencoded") {
    const auto uri{path_utils::path_to_uri(std::filesystem::path{"C:/Users/me/project/main.gh"})};
    CHECK(uri == "file:///C:/Users/me/project/main.gh");
}
#endif

} // namespace ghoti::tests
