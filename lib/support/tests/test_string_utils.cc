#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "support/string_utils.hh"

namespace ghoti::tests {

TEST_CASE("Lowercase string comptime construction") {
    STATIC_CHECK(string_utils::lowercase_str<>("a").view() == "a");
    STATIC_CHECK(string_utils::lowercase_str<>("A").view() == "a");
    STATIC_CHECK(string_utils::lowercase_str<>("").view() == "");
    STATIC_CHECK(string_utils::lowercase_str<>("asdfA_ffa").view() == "asdfa_ffa");
}

TEST_CASE("Splitting a string on a delimiter") {
    using pieces = std::vector<std::string_view>;
    CHECK(string_utils::split("", ", ").empty());
    CHECK(string_utils::split("a", ", ") == pieces{"a"});
    CHECK(string_utils::split("a, b, c", ", ") == pieces{"a", "b", "c"});
    CHECK(string_utils::split("a, b, ", ", ") == pieces{"a", "b"});
    CHECK(string_utils::split("a, , b", ", ") == pieces{"a", "", "b"});
}

} // namespace ghoti::tests
