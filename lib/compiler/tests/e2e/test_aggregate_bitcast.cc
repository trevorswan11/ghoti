#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@bitCast between an integer and an array goes through memory") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let x: u128 = (5 << 64) | 3;
            let halves: [2]u64 = @bitCast(x);
            let parts: [2]mut u64 = .{ 7, 1 };
            let joined: u128 = @bitCast(parts);
            return @intCast(halves[0] + halves[1] + (joined >> 64) + @truncate(u64, joined));
        };
    )") == 16);
}

TEST_CASE("@bitCast of an untyped integer fills every byte of an array") {
    CHECK(helpers::compile_and_run(R"(
        const all_ones = fn(): bool {
            let q: [2]u64 = @bitCast(-1);
            return q[0] == 0xFFFFFFFFFFFFFFFF and q[1] == 0xFFFFFFFFFFFFFFFF;
        };
        const split = fn(): bool {
            let mut q: [2]mut u64 = .{ 7, 7 };
            q = @bitCast(0x50000000000000003);
            return q[0] == 3 and q[1] == 5;
        };
        pub const main = fn(): i32 {
            return if (all_ones() and split()) 1 else 0;
        };
    )") == 1);
    helpers::test_checker_fail("const f = fn(): void { let q: [2]u32 = @bitCast(1 << 64); };",
                               sema::diagnostic{"integer literal is out of range for type 'u64'",
                                                sema::error::LITERAL_OUT_OF_RANGE,
                                                std::pair{0UZ, 48UZ}});
}

TEST_CASE("Assigning through a reference infers the referent's type") {
    CHECK(helpers::compile_and_run(R"(
        const Pair = struct { a: i32, b: i32 };
        const fill = fn(q: &mut [2]mut u64): void {
            q = @bitCast(0x20000000000000001);
        };
        const reset = fn(p: &mut Pair): void {
            p = .{ .a = 4, .b = 6 };
        };
        pub const main = fn(): i32 {
            let mut q: [2]mut u64 = .{ 9, 9 };
            fill(&mut q);
            let mut p: Pair = .{ .a = 0, .b = 0 };
            reset(&mut p);
            return @intCast(q[0] + q[1] * 10) + p.a + p.b;
        };
    )") == 31);
}

TEST_CASE("A rejected store points at the start of what it writes") {
    helpers::test_checker_fail(
        "const f = fn(): void { let mut q: [2]u32 = .{ 1, 2 }; q[1] = 0; };",
        sema::diagnostic{"Cannot assign to an element of a non-mutable array or slice",
                         sema::error::ASSIGNMENT_TO_CONST,
                         std::pair{0UZ, 54UZ}});
}

} // namespace ghoti::tests
