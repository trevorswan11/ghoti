#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E: module-scope pointer const from @ptrFromInt round-trips through @intFromPtr") {
    CHECK(helpers::compile_and_run(R"(
        const P: ^mut opaque = @ptrFromInt(^mut opaque, 0xDEADBEEFuz);
        pub const main := fn(): i32 {
            return if (@intFromPtr(P) == 0xDEADBEEFuz) 7 else 1;
        };
    )") == 7);
}

TEST_CASE("E2E: module-scope constexpr pointer sentinel compares equal to the same runtime pointer") {
    CHECK(helpers::compile_and_run(R"(
        constexpr IHV: ^mut opaque = @ptrFromInt(^mut opaque, 0xFFFFFFFFFFFFFFFFuz);
        pub const main := fn(): i32 {
            var h: ^mut opaque = @ptrFromInt(^mut opaque, 0xFFFFFFFFFFFFFFFFuz);
            return if (h == IHV) 7 else 1;
        };
    )") == 7);
}

TEST_CASE("E2E: module-scope usize maximum via @as(usize, -1) folds to all-ones") {
    CHECK(helpers::compile_and_run(R"(
        const MAX: usize = @as(usize, -1);
        pub const main := fn(): i32 {
            return if (MAX == 0xFFFFFFFFFFFFFFFFuz) 7 else 1;
        };
    )") == 7);
}

TEST_CASE("E2E: module-scope constexpr usize maximum via @bitCast folds to all-ones") {
    CHECK(helpers::compile_and_run(R"(
        constexpr MAX: usize = @bitCast(usize, -1i64);
        pub const main := fn(): i32 {
            return if (MAX == 0xFFFFFFFFFFFFFFFFuz) 7 else 1;
        };
    )") == 7);
}

TEST_CASE("E2E: module-scope narrow unsigned const from @as(-1) wraps to its maximum") {
    CHECK(helpers::compile_and_run(R"(
        const A: u8  = @as(u8, -1);
        const B: u16 = @as(u16, -1);
        pub const main := fn(): i32 {
            if (A != 255u8) { return 1; };
            if (B != 65535u16) { return 2; };
            return 7;
        };
    )") == 7);
}

TEST_CASE("E2E: local narrow unsigned @as / @bitCast of a negative literal wraps to its maximum") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const a: u8  = @as(u8, -1);
            const b: u16 = @as(u16, -1);
            const c: u32 = @as(u32, -1);
            const d: u8  = @bitCast(u8, -1i8);
            if (a != 255u8) { return 1; };
            if (b != 65535u16) { return 2; };
            if (c != 4294967295u32) { return 3; };
            if (d != 255u8) { return 4; };
            return 7;
        };
    )") == 7);
}

TEST_CASE("E2E: a folded @ptrFromInt module const serves as a compile-time array dimension") {
    CHECK(helpers::compile_and_run(R"(
        const N: usize = @as(usize, 4);
        const BUF_LEN: usize = @intFromPtr(@ptrFromInt(^mut opaque, N + 1uz));
        pub const main := fn(): i32 {
            var buf: [BUF_LEN]mut i32 = undefined;
            buf[4] = 7;   // valid only if BUF_LEN >= 5
            return buf[4];
        };
    )") == 7);
}

} // namespace ghoti::tests
