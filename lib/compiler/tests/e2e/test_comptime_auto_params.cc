#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("An untyped integer bound to `auto` stays comptime_int") {
    CHECK(helpers::compile_and_run(R"(
        const kind = fn(x: auto): i32 {
            return if comptime (@TypeOf(x) == comptime_int) 1 else 2;
        };
        pub const main = fn(): i32 {
            return kind(5) * 10 + kind(5i32);
        };
    )") == 12);

    // Wider than any default runtime integer, so materializing it would not fit
    CHECK(helpers::compile_and_run(R"(
        const shr = fn(x: auto): @TypeOf(x) { return x >> 60; };
        pub const main = fn(): i32 {
            return @intCast(i32, shr(5 << 60));
        };
    )") == 5);
}

TEST_CASE("An `auto` parameter read at compile time keeps a comptime_int argument") {
    CHECK(helpers::compile_and_run(R"(
        const bits = fn(x: auto): @TypeOf(x) {
            return match comptime (@typeInfo(@TypeOf(x))) {
                .comptime_int => comptime {
                    comptime let mut n = 0;
                    comptime let mut v = x;
                    while (v != 0) : (v >>= 1) { n += 1; }
                    return n;
                },
                .int => @intCast(@TypeOf(x), 64 - @clz(@as(u64, @intCast(x)))),
                _ => @compileError("not an integer"),
            };
        };
        pub const main = fn(): i32 {
            const big = bits(123456789123456798123456789);
            let small: u32 = 255;
            return @intCast(i32, big) + @intCast(i32, bits(small));
        };
    )") == 95);
}

TEST_CASE("A generic's comptime bindings do not leak into the generics it instantiates") {
    // `g`'s own `T` must not shadow `inner`'s local `T`
    CHECK(helpers::compile_and_run(R"(
        const inner = fn(x: auto): i32 {
            const T = @TypeOf(x);
            return if comptime (T == i64) 1 else 2;
        };
        const Ctor = fn(comptime n: comptime_int): type {
            return @Int(.{ .signedness = .unsigned, .bits = inner(@as(i64, n)) * 8 });
        };
        const g = fn(comptime T: type): i32 {
            _ = T;
            return @bitSizeOf(Ctor(3));
        };
        pub const main = fn(): i32 { return g(u32); };
    )") == 8);
}

TEST_CASE("A runtime `if` with a compile-time-known condition only emits its taken arm") {
    CHECK(helpers::compile_and_run(R"(
        const fits_u8 = fn(x: auto): i32 {
            if (x > 255) { return -1; }
            return @intCast(u8, x);
        };
        pub const main = fn(): i32 {
            return fits_u8(300) + fits_u8(7);
        };
    )") == 6);
}

TEST_CASE("A `match comptime` arm that returns from a comptime block types as noreturn") {
    CHECK(helpers::compile_and_run(R"(
        const pick = fn(x: auto): @TypeOf(x) {
            return match comptime (@typeInfo(@TypeOf(x))) {
                .comptime_int => comptime { return x + 1; },
                _ => x,
            };
        };
        pub const main = fn(): i32 {
            const a = pick(41);
            return a + pick(0i32);
        };
    )") == 42);
}

TEST_CASE("An inferred return type meets every return, not just the first") {
    CHECK(helpers::compile_and_run(R"(
        const clamp = fn(val: auto, low: auto, high: auto): auto {
            if (val < low) { return low; }
            if (val > high) { return high; }
            return val;
        };
        pub const main = fn(): i32 {
            let v: i64 = 500;
            const r = clamp(v, 0, 100);
            return if (@TypeOf(r) == i64) @intCast(r) else 1;
        };
    )") == 100);
}

TEST_CASE("A `@TypeOf(x)` parameter follows a comptime_int `x`") {
    CHECK(helpers::compile_and_run(R"(
        const echo = fn(x: auto, y: @TypeOf(x)): auto { return y; };
        pub const main = fn(): i32 { return @intFromFloat(i32, echo(1.5, 2)) + echo(1, 5); };
    )") == 7);
    CHECK(helpers::raised(R"(
        const echo = fn(x: auto, y: @TypeOf(x)): auto { return y; };
        pub const main = fn(): i32 { let c = echo(1, true); return 0; };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("Errors from a module that already failed still point into that module") {
    auto [ctx, idx]{helpers::type_check(
        R"(
        import "m.gh" as m;
        pub const main = fn(): i32 {
            return m.first(1) + m.second(2);
        };
    )",
        {helpers::mock_file{"m.gh",
                            R"(pub const first = fn(x: auto): i32 {
    @compileError("first failure");
    return x;
};
pub const second = fn(x: auto): i32 {
    @compileError("second failure");
    return x;
};)",
                            "m"}})};
    const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
    REQUIRE(diags);
    for (const auto& d : *diags) {
        if (!d.get_message() || !d.get_message()->contains("failure")) { continue; }
        REQUIRE(d.origin());
        CHECK(d.origin()->path.filename() == "m.gh");
    }
}

} // namespace ghoti::tests
