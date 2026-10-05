#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("E2E raw identifier: a keyword-named binding is usable") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let @"type" = 40;
            let @"match" = 2;
            return @"type" + @"match";
        };
    )") == 42);
}

TEST_CASE("E2E raw identifier: a raw name and its bare spelling denote the same symbol") {
    CHECK(helpers::compile_and_run(R"(
        const plain = 42;

        pub const main = fn(): i32 {
            return @"plain";
        };
    )") == 42);
}

TEST_CASE("E2E raw identifier: keyword-named struct fields round-trip through codegen") {
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

TEST_CASE("E2E raw identifier: a primitive spelling can name a user binding") {
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

} // namespace ghoti::tests
