#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Inline asm computes a value through a bound output") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const x: i32 = 40;
            const y: i32 = 2;
            var out: i32 = 0;
            asm {
                template: "movl %1, %0\n\taddl %2, %0",
                outputs: ("=&r" = out),
                inputs: ("r" = x, "r" = y),
                options: (volatile),
            };
            return out;
        };
    )") == 42);
}

TEST_CASE("Inline asm result slot feeds an expression") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const seed: i32 = 21;
            const doubled := asm i32 {
                template: "movl %1, %0\n\taddl %1, %0",
                outputs: ("=&r" = _),
                inputs: ("r" = seed),
                options: (volatile),
            };
            return doubled;
        };
    )") == 42);
}

} // namespace ghoti::tests
