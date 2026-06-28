#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "support/indent.hh"

namespace ghoti::tests {

TEST_CASE("Indents over time") {
    indent indent;

    SECTION("Empty indent") { CHECK(indent.current_branch() == ""); }

    SECTION("Single non-last") {
        const indent::guard g{indent, false};
        CHECK(indent.current_branch() == symbols::T_BRANCH);
    }

    SECTION("Single last") {
        const indent::guard g{indent, true};
        CHECK(indent.current_branch() == symbols::L_BRANCH);
    }

    SECTION("Nested levels") {
        const indent::guard g1{indent, false};
        {
            const indent::guard g2{indent, true};
            CHECK(indent.current_branch() ==
                  fmt::format("{}{}", symbols::VERT_BAR, symbols::L_BRANCH));
        }
    }

    SECTION("Nested levels") {
        const indent::guard g1{indent, true};
        const indent::guard g2{indent, true};
        const indent::guard g3{indent, false};
        CHECK(indent.current_branch() ==
              fmt::format("{}{}{}", symbols::EMPTY, symbols::EMPTY, symbols::T_BRANCH));
    }
}

} // namespace ghoti::tests
