#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@typeInfo of a function declaration reflects its attributes") {
    helpers::resolve_and_check(R"(
        @[inline(.always), align(32)] const fast := fn(x: i32): i32 { return x; };
        @[naked] const stub := fn(): void {};
        @[discardable] const log := fn(x: i32): i32 { return x; };
        const slow := fn(): i32 {
            @branchHint(.cold);
            return 1;
        };
        const plain := fn(): i32 { return 0; };
        constexpr {
            @assert(@typeInfo(fast).function.inline_mode == .always);
            @assert(@typeInfo(fast).function.alignment == 32);
            @assert(@typeInfo(stub).function.naked);
            @assert(@typeInfo(stub).function.inline_mode == .never);
            @assert(@typeInfo(log).function.discardable);
            @assert(@typeInfo(slow).function.cold);
            @assert(@typeInfo(plain).function.inline_mode == .default);
            @assert(!@typeInfo(plain).function.discardable);
            @assert(@typeInfo(plain).function.alignment == 0);
        }
    )");
}

TEST_CASE("@typeInfo of a bare function type reports attribute defaults") {
    helpers::resolve_and_check(R"(
        @[inline(.always), discardable] const fast := fn(x: i32): i32 { return x; };
        constexpr {
            @assert(@typeInfo(@TypeOf(fast)).function.inline_mode == .default);
            @assert(!@typeInfo(@TypeOf(fast)).function.discardable);
            @assert(@typeInfo(@TypeOf(fast)).function.alignment == 0);
        }
    )");
}

TEST_CASE("A forwarding wrapper can copy its target's discardability") {
    helpers::resolve_and_check(R"(
        @[discardable] const log := fn(x: i32): i32 { return x; };
        @[discardable(@typeInfo(log).function.discardable)]
        const forward := fn(x: i32): i32 { return log(x); };
        pub const main := fn(): i32 {
            forward(1);
            return 0;
        };
    )");
}

TEST_CASE("inline(.default) leaves inlining to the optimizer") {
    helpers::resolve_and_check(R"(
        @[inline(.default)] const f := fn(x: i32): i32 { return x; };
        constexpr { @assert(@typeInfo(f).function.inline_mode == .default); }
    )");
}

TEST_CASE("Struct field alignment is reflected and honored by @Struct") {
    helpers::resolve_and_check(R"(
        const S := struct { a: u8, @[align(16)] b: u8 };
        const Rebuilt := @Struct(@typeInfo(S).@"struct");
        constexpr {
            @assert(@typeInfo(S).@"struct".fields[0].alignment == 0);
            @assert(@typeInfo(S).@"struct".fields[1].alignment == 16);
            @assert(@alignOf(Rebuilt) == 16);
            @assert(@sizeOf(Rebuilt) == 32);
        }
    )");
    CHECK(helpers::raised(R"(
        const Bad := @Struct(.{
            .fields = ^.{ .{ .name = "a", .@"type" = u8, .alignment = 3 } },
            .is_extern = false,
            .is_packed = false,
            .backing_bits = 0,
        });
    )",
                          sema::error::CONSTEXPR_EVALUATION_FAILED));
}

} // namespace ghoti::tests
