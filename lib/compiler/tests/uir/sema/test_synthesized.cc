#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>

#include "compiler/module/memory_loader.hh"
#include "compiler/sema/error.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

using sema::error;

[[nodiscard]] auto diff_source(std::string_view text) -> helpers::uir_diff {
    mod::memory_loader loader;
    loader.add("main.gh", std::string{text});
    return helpers::diff_pipelines(loader, "main.gh", {}, {});
}

} // namespace

TEST_CASE("`A || B` merges two enums, the left keeping its values", "[uir]") {
    const auto diff{diff_source(R"(
        const Io = enum { closed, broken };
        const Own = enum { denied, broken, full };
        const Both = Io || Own;
        pub const code = fn(b: Both): i32 {
            return match (b) {
                .closed => 1,
                .broken => 2,
                .denied => 3,
                .full => 4,
            };
        };
        pub const known = fn(): Both { return .full; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A source enum widens into its merge, moved tags rewritten", "[uir]") {
    const auto diff{diff_source(R"(
        const Io = enum { closed, broken };
        const Own = enum { denied, broken, full };
        const Both = Io || Own;
        pub const from_io = fn(e: Io): Both { return e; };
        pub const from_own = fn(e: Own): Both { return e; };
        pub const known = fn(): Both { const e: Own = .denied; return e; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`||` on something that isn't an enum is reported", "[uir]") {
    const auto run{helpers::uir_rejected(R"(
        const E = enum { a };
        const Bad = E || bool;
        pub const f = fn(x: Bad): void {};
    )")};
    CHECK(run.codes() == std::vector{error::TYPE_MISMATCH});
    CHECK(run.messages().front() ==
          "'||' merges two enum types, but found 'bool'; use 'or' for a logical or");
}

TEST_CASE("`@Enum` builds an enum from its descriptor", "[uir]") {
    const auto diff{diff_source(R"(
        const Color = @Enum(.{
            .tag_type = u8,
            .fields = [2]builtin.EnumFieldInfo{
                .{ .name = "red", .value = 1 },
                .{ .name = "green", .value = 4 },
            },
            .exhaustive = true,
        });
        pub const code = fn(c: Color): i32 {
            return match (c) { .red => 10, .green => 20 };
        };
        pub const green = fn(): Color { return .green; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@Struct` and `@Union` build types from their descriptors", "[uir]") {
    const auto diff{diff_source(R"(
        const Pair = @Struct(.{
            .fields = [2]builtin.StructFieldInfo{
                .{ .name = "a", .@"type" = i32, .is_pub = true },
                .{ .name = "b", .@"type" = u8, .is_pub = true, .alignment = 8 },
            },
            .is_extern = false,
            .is_packed = false,
            .backing_bits = 0,
        });
        const Either = @Union(.{
            .fields = [2]builtin.UnionFieldInfo{
                .{ .name = "int", .@"type" = i64 },
                .{ .name = "flag", .@"type" = bool },
            },
            .is_extern = false,
            .is_packed = false,
            .tagged = true,
        });
        pub const sum = fn(p: Pair): i32 { return p.a + p.b; };
        pub const make = fn(): Pair { return .{ .a = 1, .b = 2 }; };
        pub const read = fn(e: Either): i64 {
            return match (e) { .int => |v| v, .flag => |f| if (f) 1 else 0 };
        };
        pub const size = fn(): usize { return @sizeOf(Pair); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A global's initializer takes another global's address", "[uir]") {
    const auto diff{diff_source(R"(
        const Box = struct { v: ^mut i32 };
        let mut count: i32 = 3;
        pub let mut boxed: Box = .{ .v = ^mut count };
        pub const read = fn(): i32 { return *boxed.v + 1; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A global's initializer holds a `dyn` of another global", "[uir]") {
    const auto diff{diff_source(R"(
        const Shape = interface { pub const area = fn(&self): i32; };
        const Sq = struct { side: i32 };
        impl Shape for Sq {
            pub const area = fn(&self): i32 { return self.side * self.side; };
        }
        const Holder = struct { shape: ^mut dyn Shape };
        let mut square: Sq = .{ .side = 4 };
        pub let mut held: Holder = .{ .shape = ^mut square };
        pub const area = fn(): i32 { return held.shape.area(); };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An extern function is declared for the linker and called by name", "[uir]") {
    const auto diff{diff_source(R"(
        extern const ext_take: fn(): bool;
        extern const ext_add: fn(a: i32, b: i32): i32;
        pub const f = fn(): i32 {
            if (ext_take()) { return ext_add(1, 2); }
            return 0;
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A pointer built from an integer at compile time is that address", "[uir]") {
    const auto diff{diff_source(R"(
        const Handle = ^opaque;
        pub const BAD: Handle = @ptrFromInt(Handle, @bitCast(usize, -1z));
        pub const NUL: Handle = @ptrFromInt(Handle, 16);
        pub const is_bad = fn(h: Handle): bool { return h == BAD; };
        pub const is_nul = fn(h: Handle): bool { return h == NUL; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("An extern function from a library keeps its link name and convention", "[uir]") {
    const auto diff{diff_source(R"(
        extern("kernel32") const GetLastError: extern fn() callconv(.winapi): u32;
        extern("kernel32", "CloseHandle") const close: extern fn(h: ^opaque) callconv(.winapi): i32;
        pub const last = fn(): u32 { return GetLastError(); };
        pub const shut = fn(h: ^opaque): bool { return close(h) != 0; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`.name(args)` calls the member of the type it is used as", "[uir]") {
    const auto diff{diff_source(R"(
        const R = union(enum) {
            ok: i32,
            err: u8,
            pub const fail = fn(e: u8): @This() { return .{ .err = e }; };
            pub const of = fn(v: i32): @This() { return .{ .ok = v }; };
        };
        pub const pick = fn(x: i32): R {
            if (x < 0) { return .fail(7); }
            return .of(x * 2);
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`!p` tests a pointer against null", "[uir]") {
    const auto diff{diff_source(R"(
        pub const isnull = fn(h: ^opaque): bool { return !h; };
        pub const some = fn(h: ^opaque): i32 { return if (!h) 1 else 2; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("Memory builtins take each buffer apart where it is evaluated", "[uir]") {
    const auto diff{diff_source(R"(
        pub const copy = fn(dst: []mut u8, src: []u8, n: usize): void {
            @memcpy(dst[0..n], src[0..n]);
        };
        pub const shift = fn(xs: []mut i32): void { @memmove(xs[0..2], xs[1..3]); };
        pub const clear = fn(): u8 {
            let mut buf: [8]mut u8 = undefined;
            @memset(buf[0..], 0);
            return buf[3];
        };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A global constant holding another global's address", "[uir]") {
    const auto diff{diff_source(R"(
        let mut p: i32 = 1;
        pub const q: ^i32 = ^p;
        pub const read = fn(): i32 { return *q; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A typed local `const` takes a `.name(args)` call's value", "[uir]") {
    const auto diff{diff_source(R"(
        const P = struct {
            x: i32,
            pub const make = fn(v: i32): @This() { return .{ .x = v * 2 }; };
        };
        pub const f = fn(): i32 {
            const p: P = .make(3);
            return p.x;
        };
    )")};
    INFO(diff.detail);
    // The old emitter makes the call at runtime, which UIR folds
    CHECK(diff.status == helpers::uir_diff_status::FOLD_PLACEMENT);
}

TEST_CASE("`@Struct` and `@Union` build packed and enum-tagged types", "[uir]") {
    const auto diff{diff_source(R"(
        const Bits = @Struct(.{
            .fields = [2]builtin.StructFieldInfo{
                .{ .name = "lo", .@"type" = u3, .is_pub = true },
                .{ .name = "hi", .@"type" = u5, .is_pub = true },
            },
            .is_extern = false,
            .is_packed = true,
            .backing_bits = 8,
        });
        const K = enum { a, b };
        const U = @Union(.{
            .fields = [2]builtin.UnionFieldInfo{
                .{ .name = "b", .@"type" = u8 },
                .{ .name = "a", .@"type" = i32 },
            },
            .is_extern = false,
            .is_packed = false,
            .tagged = true,
            .tag_type = K,
        });
        pub const hi = fn(b: Bits): u5 { return b.hi; };
        pub const tag = fn(u: U): K { let k: K = u; return k; };
    )")};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("`@Union` checks the enum its tag comes from", "[uir]") {
    const auto field{GENERATE(std::string_view{".{ .name = \"c\", .@\"type\" = u8 }"},
                              std::string_view{".{ .name = \"a\", .@\"type\" = u8 }"})};
    INFO(field);
    const auto run{helpers::uir_rejected(fmt::format(R"(
        const K = enum {{ a, b }};
        const U = @Union(.{{
            .fields = [1]builtin.UnionFieldInfo{{ {} }},
            .is_extern = false,
            .is_packed = false,
            .tagged = true,
            .tag_type = K,
        }});
        pub const f = fn(u: U): void {{ _ = u; }};
    )",
                                                     field))};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
}

} // namespace ghoti::tests
