#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@truncate truncates high bits modularly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let val: u16 = 0x1234u16;
            let narrow: u8 = @truncate(u8, val);
            return if (narrow == 0x34u8) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("@truncate with context-inferred target") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let val: u32 = 0xAABBCCDDu32;
            let narrow: u16 = @truncate(val);
            return if (narrow == 0xCCDDu16) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("@truncate const-evaluates at compile time") {
    CHECK(helpers::compile_and_run(R"(
        const VAL: u32 = 0x12345678u32;
        const NARROW: u8 = @truncate(u8, VAL);
        pub const main = fn(): i32 {
            return if (NARROW == 0x78u8) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("@truncate to the same width is a no-op reinterpret") {
    CHECK(helpers::compile_and_run(R"(
        const id = fn(comptime T: type, x: T): T { return @truncate(T, x); };
        const SAME: u128 = @truncate(u128, 0xFFu128 << 120);
        pub const main = fn(): i32 {
            let wide: u128 = (0xABu128 << 120) | 7;
            if (id(u128, wide) != wide) { return 1; }
            if (SAME != 0xFFu128 << 120) { return 2; }
            let all_ones: u32 = 0xFFFFFFFFu32;
            let neg: i32 = @truncate(i32, all_ones);
            return if (neg == -1) 0 else 3;
        };
    )") == 0);
}

} // namespace ghoti::tests
