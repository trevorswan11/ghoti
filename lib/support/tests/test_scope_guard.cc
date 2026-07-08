#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "support/scope_guard.hh"

namespace ghoti::tests {

namespace {

struct custom_stack {
    std::vector<i32> elements{};

    auto push(i32 val) -> void { elements.push_back(val); }
    auto pop() -> void {
        if (!elements.empty()) { elements.pop_back(); }
    }
};

struct complex_entry {
    i32         id{0};
    std::string label{};
};

} // namespace

TEST_CASE("scope_guard with std::vector default element") {
    std::vector<i32> vec;
    CHECK(vec.empty());

    {
        const scope_guard g{vec};
        REQUIRE(vec.size() == 1);
        CHECK(vec.back() == 0);
    }
    CHECK(vec.empty());
}

TEST_CASE("scope_guard with variadic emplace_back arguments") {
    std::vector<complex_entry> vec;
    CHECK(vec.empty());

    {
        const scope_guard g{vec, 101, "first_scope"};
        REQUIRE(vec.size() == 1);
        CHECK(vec.back().id == 101);
        CHECK(vec.back().label == "first_scope");

        {
            const scope_guard g_nested{vec, 202, "nested_scope"};
            REQUIRE(vec.size() == 2);
            CHECK(vec.back().id == 202);
            CHECK(vec.back().label == "nested_scope");
        }

        REQUIRE(vec.size() == 1);
        CHECK(vec.back().id == 101);
    }
    CHECK(vec.empty());
}

TEST_CASE("scope_guard with custom stack push/pop") {
    custom_stack stack;
    CHECK(stack.elements.empty());

    {
        const scope_guard g{stack, 42};
        REQUIRE(stack.elements.size() == 1);
        CHECK(stack.elements.back() == 42);
    }
    CHECK(stack.elements.empty());
}

TEST_CASE("scope_guard move construction semantics") {
    std::vector<i32> vec;

    {
        scope_guard g1{vec, 99};
        CHECK(vec.size() == 1);

        {
            scope_guard g2{std::move(g1)};
            CHECK(vec.size() == 1);
        }
        // g2 destructs here and pops vec
        CHECK(vec.empty());
    }
    // g1 destructs here as moved-from, must not pop again
    CHECK(vec.empty());
}

TEST_CASE("scope_guard move assignment semantics") {
    std::vector<i32> vec1;
    std::vector<i32> vec2;

    {
        scope_guard g1{vec1, 10};
        scope_guard g2{vec2, 20};

        CHECK(vec1.size() == 1);
        CHECK(vec2.size() == 1);

        // Assigning g1 to g2: g2 pops vec2 first, then adopts vec1
        g2 = std::move(g1);
        CHECK(vec2.empty());
        CHECK(vec1.size() == 1);
    }
    // g2 destructs and pops vec1, g1 is moved-from
    CHECK(vec1.empty());
    CHECK(vec2.empty());
}

} // namespace ghoti::tests
