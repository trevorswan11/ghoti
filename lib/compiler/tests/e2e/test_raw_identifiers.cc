#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("A keyword-named binding is usable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let @"type" = 40;
            let @"match" = 2;
            return @"type" + @"match";
        };
    )") == 42);
}

TEST_CASE("A raw name and its bare spelling denote the same symbol") {
    CHECK(helpers::compile_and_run(R"(
        const plain = 42;

        pub const main = fn(): i32 {
            return @"plain";
        };
    )") == 42);
}

TEST_CASE("Keyword-named struct fields round-trip through codegen") {
    CHECK(helpers::compile_and_run(R"(
        const Box = struct {
            @"struct": i32,
            @"fn": i32,
        };

        pub const main = fn(): i32 {
            let b: Box = .{ .@"struct" = 30, .@"fn" = 12 };
            return b.@"struct" + b.@"fn";
        };
    )") == 42);
}

TEST_CASE("A primitive spelling can name a user binding") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let @"i32": i32 = 42;
            return @"i32";
        };
    )") == 42);
}

TEST_CASE(
    "E2E raw identifier: a declared primitive spelling doesn't shadow the primitive keyword") {
    CHECK(helpers::compile_and_run(R"(
        const @"u8" = struct {
            pub const twice = fn(v: u8): u8 { return v * 2; };
        };
        const @"i64" = struct { k: i32 };

        const Wrap = struct {
            const @"i32" = struct { k: i32 };
            pub const get = fn(v: i32): i32 { return v + 1; };
        };

        pub const main = fn(): i32 {
            let s: @"i64" = .{ .k = 20 };
            let w: i64 = 1;
            return @"u8".twice(10) + s.k + Wrap.get(0) + @intCast(w);
        };
    )") == 42);
}

TEST_CASE("A folded const named after a primitive can read the primitive") {
    CHECK(helpers::compile_and_run(R"(
        const @"f80" = match comptime (@targetArch()) {
            .x86_64, .x86 => f80,
            _ => u80,
        };
        const @"f32" = f32;
        const @"bool" = bool;
        const @"usize" = usize;
        const @"c_int" = c_int;

        pub const main = fn(): i32 {
            let x: @"f80" = @bitCast(@as(u80, 0));
            return @intCast(@bitSizeOf(@TypeOf(x)) + @sizeOf(@"f32") + @sizeOf(@"bool") +
                @sizeOf(@"usize") / @sizeOf(usize) + @sizeOf(@"c_int"));
        };
    )") == 90);
}

TEST_CASE("Folding reads a raw name as the decl, not the primitive") {
    CHECK(helpers::compile_and_run(R"(
        const @"u8" = struct { a: u64, b: u64 };
        const N: usize = @sizeOf(@"u8");
        const size = fn(comptime T: type): i32 { return @intCast(@sizeOf(T)); };

        pub const main = fn(): i32 {
            const @"f32" = u8;
            return @intCast(N) + size(f32) * 10 + size(@"f32") * 100;
        };
    )") == 156);
}

} // namespace ghoti::tests
