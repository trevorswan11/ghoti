#include <algorithm>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto error_mentions(std::string_view source, std::string_view needle) -> bool {
    auto [ctx, idx]{helpers::expect_compile_error(source)};
    const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
    return diags && std::ranges::any_of(*diags, [&](const auto& d) {
               const auto& msg{d.get_message()};
               return msg && msg->contains(needle);
           });
}

} // namespace

TEST_CASE("An inferred cast's target never types its operand") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut s: u128 = 256;
            let e: i32 = -2;
            s >>= @intCast(4 - e);
            let x: u64 = @intCast(4 - e);
            return @intCast(s + x);
        };
    )") == 10);
    CHECK(helpers::compile_and_run(R"(
        const shift = fn(v: u64, e: i32): u64 {
            return v >> @intCast(8 - e);
        };

        pub const main = fn(): i32 {
            let w: u8 = @truncate(300 + shift(1024, 0) * 0);
            let b: u32 = @bitCast(-42);
            let s: i32 = @bitCast(b);
            return @intCast(shift(1024, 0) + w + @intCast(u8, s + 42));
        };
    )") == 48);
}

TEST_CASE("An untyped literal reinterprets at the target's width") {
    CHECK(helpers::compile_and_run(R"(
        const one = fn(): f128 { return @bitCast(0x3fff0000000000000000000000000000); };
        const half = fn(): f16 { return @bitCast(0x3800); };
        const ones = fn(): u128 { return @bitCast(-1); };

        pub const main = fn(): i32 {
            let one_bits: u128 = @bitCast(one());
            let a: i32 = if (one_bits >> 112 == 0x3fff) 3 else 0;
            let b: i32 = @intFromFloat(half() * 4.0);
            let c: i32 = if (ones() == 0xffffffffffffffffffffffffffffffff) 1 else 0;
            return a + b + c;
        };
    )") == 6);
}

TEST_CASE("A compile-time typed value casts to comptime_int") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const x: i32 = 5;
            const big = @as(comptime_int, x) * 1000000000000;
            const via_int_cast = @intCast(comptime_int, x);
            const narrow: u8 = 255;
            const widened: comptime_int = narrow;
            return @intCast(big / 1000000000000 + via_int_cast + widened - 250);
        };
    )") == 15);
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const f: f32 = 2.5;
            const up = @intFromFloat(comptime_int, f * 2.0);
            const down = @intFromFloat(comptime_int, -f);
            return up + down + 10;
        };
    )") == 13);
}

TEST_CASE("A compile-time typed value casts to comptime_float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const f: f32 = 2.5;
            const d: f64 = 0.1;
            const i: i32 = 6;
            const a = @as(comptime_float, f);
            const b = @floatCast(comptime_float, f);
            const c = @floatFromInt(comptime_float, i);
            const exact = if (@as(comptime_float, d) == 2.5) 1 else 0;
            return @intFromFloat(i32, a * 2.0 + b * 2.0 + c) + exact;
        };
    )") == 16);
    // An `f64` 0.1 widens exactly, so it is not the untyped 0.1 in any context
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            const d: f64 = 0.1;
            const folded = if (@as(comptime_float, d) == 0.1) 1 else 0;
            return folded + if (@as(comptime_float, d) == 0.1) 10 else 20;
        };
    )") == 20);
}

TEST_CASE("A cast to an untyped number needs a compile-time operand") {
    CHECK(error_mentions(R"(
        const f = fn(x: i32): i32 {
            let y = @intCast(comptime_int, x);
            return 1;
        };
        pub const main = fn(): i32 { let mut a: i32 = 7; a += 1; return f(a); };
    )",
                         "a runtime 'i32' cannot become 'comptime_int'"));
    CHECK(error_mentions(R"(
        const f = fn(x: i32): i32 {
            let y = @as(comptime_int, x);
            return 1;
        };
        pub const main = fn(): i32 { let mut a: i32 = 7; a += 1; return f(a); };
    )",
                         "a runtime 'i32' cannot become 'comptime_int'"));
    CHECK(error_mentions(R"(
        const f = fn(x: i32): i32 {
            let y: comptime_int = x;
            return 1;
        };
        pub const main = fn(): i32 { let mut a: i32 = 7; a += 1; return f(a); };
    )",
                         "a runtime 'i32' cannot become 'comptime_int'"));
    CHECK(error_mentions(R"(
        const f = fn(x: f32): i32 {
            let y = @floatCast(comptime_float, x);
            return 1;
        };
        pub const main = fn(): i32 { let mut a: f32 = 7.0; a += 1.0; return f(a); };
    )",
                         "a runtime 'f32' cannot become 'comptime_float'"));
    CHECK(error_mentions(R"(
        const f = fn(x: f32): i32 {
            let y = @intFromFloat(comptime_int, x);
            return 1;
        };
        pub const main = fn(): i32 { let mut a: f32 = 7.0; a += 1.0; return f(a); };
    )",
                         "a runtime 'f32' cannot become 'comptime_int'"));
    CHECK(error_mentions(R"(
        const f = fn(x: i32): i32 {
            let y = @floatFromInt(comptime_float, x);
            return 1;
        };
        pub const main = fn(): i32 { let mut a: i32 = 7; a += 1; return f(a); };
    )",
                         "a runtime 'i32' cannot become 'comptime_float'"));
}

TEST_CASE("An untyped number has no width or bit layout to cast through") {
    CHECK(error_mentions(R"(
        pub const main = fn(): i32 {
            const x: i32 = 5;
            const y = @truncate(comptime_int, x);
            return y;
        };
    )",
                         "has no width to truncate to"));
    CHECK(error_mentions(R"(
        pub const main = fn(): i32 {
            const x: i32 = 5;
            const y = @bitCast(comptime_int, x);
            return y;
        };
    )",
                         "has no bit layout"));
}

TEST_CASE("An untyped annotation on a 'let mut' needs 'comptime'") {
    CHECK(error_mentions(R"(
        pub const main = fn(): i32 {
            let mut y: comptime_int = 0;
            y += 1;
            return y;
        };
    )",
                         "use 'comptime let mut'"));
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            comptime let mut y: comptime_int = 2;
            y += 1;
            let mut z = 2;
            z += 1;
            return y + z;
        };
    )") == 6);
}

} // namespace ghoti::tests
