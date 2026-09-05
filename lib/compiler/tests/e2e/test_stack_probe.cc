#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("a stack frame spanning many guard pages is fully addressable") {
    CHECK(helpers::compile_and_run(R"(
        const touch := fn(p: ^mut u8, v: u8): void { *p = v; };
        pub const main := fn(): i32 {
            var buf: [200000]u8 = undefined;
            touch(^mut buf[0], 3u8);
            touch(^mut buf[199999], 7u8);
            return @as(i32, buf[0]) + @as(i32, buf[199999]);
        };
    )") == 10);
}

TEST_CASE("a stack frame just past one guard page is fully addressable") {
    CHECK(helpers::compile_and_run(R"(
        const touch := fn(p: ^mut u8, v: u8): void { *p = v; };
        pub const main := fn(): i32 {
            var a: [5000]u8 = undefined;
            var b: [5000]u8 = undefined;
            touch(^mut a[0], 1u8);
            touch(^mut a[4999], 2u8);
            touch(^mut b[0], 4u8);
            touch(^mut b[4999], 8u8);
            return @as(i32, a[0]) + @as(i32, a[4999]) + @as(i32, b[0]) + @as(i32, b[4999]);
        };
    )") == 15);
}

} // namespace ghoti::tests
