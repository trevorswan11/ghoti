#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("u0 holds only zero and takes no space") {
    CHECK(helpers::compile_and_run(R"(
        const S = struct { a: u0, b: u32 };
        pub const main = fn(): i32 {
            if comptime (@sizeOf(u0) != 0 or @bitSizeOf(u0) != 0 or @sizeOf(S) != 4 or
                         @sizeOf([4]u0) != 0) {
                return 1;
            }
            let z: u0 = 0;
            let mut s: S = .{ .a = z, .b = 7 };
            s.a = 0;
            let w: u32 = s.a;
            let arr: [4]u0 = .{ 0, 0, 0, 0 };
            const wrapped: u0 = z +% z;
            return if (w + s.b == 7 and arr[2] == 0 and wrapped == z) 0 else 2;
        };
    )") == 0);
}

TEST_CASE("u0 converts to and from wider integers") {
    CHECK(helpers::compile_and_run(R"(
        const narrow = fn(x: u32): u0 { return @intCast(x); };
        pub const main = fn(): i32 {
            let y: u32 = 5;
            const z = narrow(0);
            return @intCast(y >> z);
        };
    )") == 5);
    helpers::expect_compile_error("pub const main = fn(): i32 { let a: u0 = 1; return 0; };");
}

TEST_CASE("@Int builds u0 but no zero-bit signed integer") {
    CHECK(helpers::compile_and_run(R"(
        const T = @Int(.{ .signedness = .unsigned, .bits = 0 });
        pub const main = fn(): i32 { return if comptime (T == u0) 0 else 1; };
    )") == 0);
    CHECK(helpers::raised("const T = @Int(.{ .signedness = .signed, .bits = 0 });",
                          sema::error::LITERAL_OUT_OF_RANGE));
    CHECK(helpers::raised("const T = @Int(.{ .signedness = .unsigned, .bits = 65536 });",
                          sema::error::LITERAL_OUT_OF_RANGE));
    helpers::expect_compile_error("pub const main = fn(): i32 { let a: i0 = 0; return 0; };");
}

} // namespace ghoti::tests
