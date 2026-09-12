#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("@truncate truncates high bits modularly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const val: u16 = 0x1234u16;
            const narrow: u8 = @truncate(u8, val);
            return if (narrow == 0x34u8) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("@truncate with context-inferred target") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const val: u32 = 0xAABBCCDDu32;
            const narrow: u16 = @truncate(val);
            return if (narrow == 0xCCDDu16) 0 else 1;
        };
    )") == 0);
}

TEST_CASE("@truncate const-evaluates at compile time") {
    CHECK(helpers::compile_and_run(R"(
        constexpr VAL: u32 = 0x12345678u32;
        constexpr NARROW: u8 = @truncate(u8, VAL);
        pub const main := fn(): i32 {
            return if (NARROW == 0x78u8) 0 else 1;
        };
    )") == 0);
}

} // namespace ghoti::tests
