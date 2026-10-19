#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a parameter written in a loop body is read fresh by the loop condition") {
    SECTION("plain function") {
        CHECK(helpers::compile_and_run(R"(
            const count_down = fn(value: u32): u32 {
                let mut steps: u32 = 0;
                while (value != 0) {
                    value -= 1;
                    steps += 1;
                }
                return steps + value;
            };
            pub const main = fn(): i32 { return @intCast(count_down(5)); };
        )") == 5);
    }
    SECTION("generic instantiation") {
        CHECK(helpers::compile_and_run(R"(
            const count_down = fn(comptime T: type, value: T): T {
                let mut steps: T = 0;
                while (value != 0) : (steps += 1) {
                    value -= 1;
                }
                return steps;
            };
            pub const main = fn(): i32 { return @intCast(count_down(u32, 7)); };
        )") == 7);
    }
    SECTION("written through its address") {
        CHECK(helpers::compile_and_run(R"(
            const dec = fn(p: ^mut u32): void { *p -= 1; };
            const count_down = fn(value: u32): u32 {
                let mut steps: u32 = 0;
                while (value != 0) {
                    dec(^mut value);
                    steps += 1;
                }
                return steps;
            };
            pub const main = fn(): i32 { return @intCast(count_down(4)); };
        )") == 4);
    }
    SECTION("a field of a struct parameter") {
        CHECK(helpers::compile_and_run(R"(
            const P = struct { n: i32 };
            const drain = fn(p: P): i32 {
                let mut steps: i32 = 0;
                while (p.n > 0) {
                    p.n -= 2;
                    steps += 1;
                }
                return steps;
            };
            pub const main = fn(): i32 { return drain(.{ .n = 6 }); };
        )") == 3);
    }
}

} // namespace ghoti::tests
