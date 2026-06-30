#include <catch2/catch_test_macros.hpp>

#include "support/string_utils.hh"

namespace ghoti::tests {

TEST_CASE("Lowercase string constexpr construction") {
    STATIC_CHECK(string_utils::lowercase_str<>("a").view() == "a");
    STATIC_CHECK(string_utils::lowercase_str<>("A").view() == "a");
    STATIC_CHECK(string_utils::lowercase_str<>("").view() == "");
    STATIC_CHECK(string_utils::lowercase_str<>("asdfA_ffa").view() == "asdfa_ffa");
}

} // namespace ghoti::tests
