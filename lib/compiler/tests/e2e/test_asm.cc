#include <catch2/catch_test_macros.hpp>

#include "ghoti/config.h"
#include "helpers/codegen.hh"

namespace ghoti::tests {

#if GHOTI_ASM_HOST_X86_64

TEST_CASE("E2E asm (x86-64): value flows through a bound output") {
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

TEST_CASE("E2E asm (x86-64): result slot feeds an expression") {
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

#elif GHOTI_ASM_HOST_AARCH64

TEST_CASE("E2E asm (aarch64): value flows through a bound output") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const x: i32 = 40;
            const y: i32 = 2;
            var out: i32 = 0;
            asm {
                template: "add %0, %1, %2",
                outputs: ("=r" = out),
                inputs: ("r" = x, "r" = y),
                options: (volatile),
            };
            return out;
        };
    )") == 42);
}

TEST_CASE("E2E asm (aarch64): result slot feeds an expression") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const seed: i32 = 21;
            const doubled := asm i32 {
                template: "add %0, %1, %1",
                outputs: ("=r" = _),
                inputs: ("r" = seed),
                options: (volatile),
            };
            return doubled;
        };
    )") == 42);
}

#endif

} // namespace ghoti::tests
