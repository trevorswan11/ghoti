#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@memcpy copies a slice's worth of bytes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var dst := [4uz]mut u8{ 0, 0, 0, 0 };
            const src := [4uz]u8{ 10, 20, 30, 40 };
            @memcpy(dst[0..4], src[0..4]);
            return @as(i32, dst[0]) + @as(i32, dst[1]) + @as(i32, dst[2]) + @as(i32, dst[3]);
        };
    )") == 100);
}

TEST_CASE("@memcpy scales the byte length by the element size") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var dst := [3uz]mut u32{ 0, 0, 0 };
            const src := [3uz]u32{ 1, 2, 3 };
            @memcpy(dst[0..3], src[0..3]);
            return @as(i32, dst[0]) + @as(i32, dst[1]) + @as(i32, dst[2]);
        };
    )") == 6);
}

TEST_CASE("@memset fills a mutable slice with a byte") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var buf := [5uz]mut u8{ 1, 2, 3, 4, 5 };
            @memset(buf[1..4], 0u8);
            return @as(i32, buf[0]) + @as(i32, buf[1]) + @as(i32, buf[2]) + @as(i32, buf[3]) +
                   @as(i32, buf[4]);
        };
    )") == 6);
}

TEST_CASE("@memmove handles a forward-overlapping copy") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a := [5uz]mut u8{ 1, 2, 3, 4, 5 };
            @memmove(a[1..5], a[0..4]); // a becomes { 1, 1, 2, 3, 4 }
            return @as(i32, a[0]) * 10000 + @as(i32, a[1]) * 1000 + @as(i32, a[2]) * 100 +
                   @as(i32, a[3]) * 10 + @as(i32, a[4]);
        };
    )") == 11'234);
}

TEST_CASE("@memmove handles a backward-overlapping copy") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a := [5uz]mut u8{ 1, 2, 3, 4, 5 };
            @memmove(a[0..4], a[1..5]); // a becomes { 2, 3, 4, 5, 5 }
            return @as(i32, a[0]) * 10000 + @as(i32, a[1]) * 1000 + @as(i32, a[2]) * 100 +
                   @as(i32, a[3]) * 10 + @as(i32, a[4]);
        };
    )") == 23'455);
}

TEST_CASE("@memcpy rejects an immutable destination") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const dst := [2uz]u8{ 0, 0 };
            const src := [2uz]u8{ 1, 2 };
            @memcpy(dst[0..2], src[0..2]);
            return 0;
        };
    )");
}

} // namespace ghoti::tests
