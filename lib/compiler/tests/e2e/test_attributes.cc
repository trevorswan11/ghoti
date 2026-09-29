#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("An over-aligned struct field keeps its value and offset at runtime") {
    CHECK(helpers::compile_and_run(R"(
        const S := struct { a: u8, @[align(16)] b: u8, c: u8 };
        const Outer := struct { tag: u8, inner: S };

        var global_s: S = .{ .a = 1, .b = 2, .c = 3 };

        pub const main := fn(): i32 {
            var o: Outer = .{ .tag = 7, .inner = .{ .a = 4, .b = 5, .c = 6 } };
            o.inner.b += global_s.b;

            const bp: ^mut u8 = ^mut o.inner.b;
            const sp: ^mut S = @fieldParentPtr(S, "b", bp);
            const base: usize = @intFromPtr(sp);
            const b_offset: usize = @intFromPtr(bp) - base;
            if (base % 16 != 0 or b_offset != 16) { return 1; }
            return @as(i32, o.tag) + @as(i32, sp.a) + @as(i32, sp.b) + @as(i32, o.inner.c);
        };
    )") == 7 + 4 + 7 + 6);
}

TEST_CASE("A local's alignment in a generic body folds per instantiation") {
    CHECK(helpers::compile_and_run(R"(
        const misaligned := fn(T: type): i32 {
            @[align(if (@sizeOf(T) > 4) 64 else 32)]
            var buf: [4]u8 = undefined;
            const addr: usize = @intFromPtr(^buf);
            return if (addr % (if (@sizeOf(T) > 4) 64 else 32) == 0) 0 else 1;
        };
        pub const main := fn(): i32 { return misaligned(i32) + misaligned(i64) * 2; };
    )") == 0);
}

TEST_CASE("@[inline(.always)] functions still compute the right result") {
    CHECK(helpers::compile_and_run(R"(
        @[inline(.always)] const twice := fn(x: i32): i32 { return x * 2; };
        pub const main := fn(): i32 { return twice(21); };
    )") == 42);
}

} // namespace ghoti::tests
