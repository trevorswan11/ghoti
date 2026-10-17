#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "support/sorted_vector_map.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto keys_of(const sorted_vector_map<usize, std::string>& map) -> std::vector<usize> {
    std::vector<usize> keys;
    for (const auto& [key, _] : map) { keys.emplace_back(key); }
    return keys;
}

} // namespace

TEST_CASE("Sorted vector map keeps keys ordered for any insertion order") {
    sorted_vector_map<usize, std::string> map;
    map.insert_or_assign(5, "five");
    map.insert_or_assign(1, "one");
    map.insert_or_assign(9, "nine");
    map.insert_or_assign(3, "three");

    CHECK(keys_of(map) == std::vector<usize>{1, 3, 5, 9});
    CHECK(map.size() == 4);
}

TEST_CASE("Sorted vector map lookup") {
    sorted_vector_map<usize, std::string> map;
    CHECK(map.empty());
    CHECK_FALSE(map.find(0));

    map.insert_or_assign(2, "two");
    map.insert_or_assign(4, "four");

    const auto two{map.find(2)};
    REQUIRE(two);
    CHECK(*two == "two");
    CHECK_FALSE(map.find(3));
    CHECK_FALSE(map.find(5));
    CHECK(map.contains(4));
}

TEST_CASE("Sorted vector map last write for a key wins") {
    sorted_vector_map<usize, std::string> map;
    map.insert_or_assign(1, "first");
    map.insert_or_assign(7, "tail");
    map.insert_or_assign(1, "second");
    map.insert_or_assign(7, "tail again");

    CHECK(map.size() == 2);
    CHECK(*map.find(1) == "second");
    CHECK(*map.find(7) == "tail again");
}

TEST_CASE("Sorted vector map values are mutable through find and iteration") {
    sorted_vector_map<usize, i32> map;
    map.insert_or_assign(1, 10);
    map.insert_or_assign(2, 20);

    *map.find(1) += 1;
    for (auto& [_, value] : map) { value *= 2; }

    CHECK(*map.find(1) == 22);
    CHECK(*map.find(2) == 40);
}

} // namespace ghoti::tests
