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

TEST_CASE("A rejected store points at the start of what it writes") {
    helpers::test_checker_fail(
        "const f = fn(): void { let mut q: [2]u32 = .{ 1, 2 }; q[1] = 0; };",
        sema::diagnostic{"Cannot assign to an element of a non-mutable array or slice",
                         sema::error::ASSIGNMENT_TO_CONST,
                         std::pair{0UZ, 54UZ}});
}

} // namespace ghoti::tests
