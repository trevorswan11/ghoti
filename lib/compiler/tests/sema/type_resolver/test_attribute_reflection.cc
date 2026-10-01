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
        comptime {
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
        comptime {
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

TEST_CASE("@typeInfo of a generic function reflects its parameter-independent attributes") {
    helpers::resolve_and_check(R"(
        @[inline(.always), discardable, align(16)]
        const g := fn(T: type, x: T): T { return x; };
        const pick := fn(comptime N: usize): usize {
            @branchHint(.cold);
            return N;
        };
        comptime {
            @assert(@typeInfo(g).function.inline_mode == .always);
            @assert(@typeInfo(g).function.discardable);
            @assert(@typeInfo(g).function.alignment == 16);
            @assert(@typeInfo(pick).function.cold);
        }
        pub const main := fn(): i32 {
            let a := pick(2);
            return g(i32, 1);
        };
    )");
}

TEST_CASE("A generic function sees its own instantiation's attributes") {
    helpers::resolve_and_check(R"(
        @[inline(if (N > 4) .never else .always)]
        const f := fn(comptime N: usize): usize {
            comptime {
                @assert(N <= 4 or @typeInfo(f).function.inline_mode == .never);
                @assert(N > 4 or @typeInfo(f).function.inline_mode == .always);
            }
            return N;
        };
        pub const main := fn(): i32 {
            let a := f(2);
            let b := f(8);
            let c := f(3);
            return 0;
        };
    )");
}

TEST_CASE("Reflecting a parameter-dependent attribute outside an instantiation is an error") {
    CHECK(helpers::raised(R"(
        @[inline(if (N > 4) .never else .always)]
        const f := fn(comptime N: usize): usize { return N; };
        comptime { @assert(@typeInfo(f).function.inline_mode == .always); }
    )",
                          sema::error::ILLEGAL_ATTRIBUTE));
}

TEST_CASE("@typeInfo of a member function reflects its attributes") {
    helpers::resolve_and_check(R"(
        const S := struct {
            @[inline(.never), align(32)] pub const f := fn(): i32 { return 1; };
        };
        comptime {
            @assert(@typeInfo(S.f).function.inline_mode == .never);
            @assert(@typeInfo(S.f).function.alignment == 32);
        }
    )");
}

TEST_CASE("@typeInfo of a type constructor's member reflects that instantiation's attributes") {
    helpers::resolve_and_check(R"(
        const Box := fn(T: type): type {
            return struct {
                v: T,
                @[inline(if (@sizeOf(T) > 4) .never else .always), discardable(@sizeOf(T) == 1)]
                pub const get := fn(self): T { return self.v; };
                @[align(16)] pub const plain := fn(): i32 { return 1; };
            };
        };
        comptime {
            @assert(@typeInfo(Box(u8).plain).function.alignment == 16);
            @assert(@typeInfo(Box(u8).get).function.inline_mode == .always);
            @assert(@typeInfo(Box(u8).get).function.discardable);
            @assert(@typeInfo(Box(i64).get).function.inline_mode == .never);
            @assert(!@typeInfo(Box(i64).get).function.discardable);
        }
    )");
}

TEST_CASE("inline(.default) leaves inlining to the optimizer") {
    helpers::resolve_and_check(R"(
        @[inline(.default)] const f := fn(x: i32): i32 { return x; };
        comptime { @assert(@typeInfo(f).function.inline_mode == .default); }
    )");
}

TEST_CASE("Struct field alignment is reflected and honored by @Struct") {
    helpers::resolve_and_check(R"(
        const S := struct { a: u8, @[align(16)] b: u8 };
        const Rebuilt := @Struct(@typeInfo(S).@"struct");
        comptime {
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
                          sema::error::COMPTIME_EVALUATION_FAILED));
}

} // namespace ghoti::tests
