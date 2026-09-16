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

TEST_CASE("@returnAddress execution and caller return address capture") {
    SECTION("returns a non-zero address") {
        CHECK(helpers::compile_and_run(R"(
            pub const get_ret_addr := fn(): usize {
                return @returnAddress();
            };

            pub const main := fn(): i32 {
                const addr := get_ret_addr();
                if (addr != 0) {
                    return 42;
                } else {
                    return 0;
                };
            };
        )") == 42);
    }

    SECTION("captures distinct return addresses from different callers") {
        CHECK(helpers::compile_and_run(R"(
            pub const get_ret_addr := fn(): usize {
                return @returnAddress();
            };

            pub const caller_a := fn(): usize {
                return get_ret_addr();
            };

            pub const caller_b := fn(): usize {
                return get_ret_addr();
            };

            pub const main := fn(): i32 {
                const a := caller_a();
                const b := caller_b();
                if (a != 0 and b != 0 and a != b) {
                    return 42;
                } else {
                    return 0;
                };
            };
        )") == 42);
    }
}

} // namespace ghoti::tests
