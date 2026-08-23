#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A comment as the first statement in a function body does not break parsing") {
    CHECK(helpers::compile_and_run(R"(
        const X := 1;

        pub const main := fn(): i32 {
            // an ordinary comment
            return 0;
        };
    )") == 0);
}

TEST_CASE("Comments in every plausible statement position are all skipped transparently") {
    CHECK(helpers::compile_and_run(R"(
        const X := 1;

        // leading comment
        pub const main := fn(): i32 { // trailing on brace
            // comment before decl
            var sum: i32 = 0; // trailing
            // comment between statements
            if (sum == 0) { // comment in if header context
                sum = sum + 1;
                // comment before closing brace
            }
            for (0..3) |v| { // comment in for header
                sum = sum + v;
            }
            match (sum) {
                // comment inside match
                4 => { sum = sum + 100; },
                _ => {},
            }
            return sum;
            // trailing comment before closing brace
        };
    )") == 104);
}

} // namespace ghoti::tests
