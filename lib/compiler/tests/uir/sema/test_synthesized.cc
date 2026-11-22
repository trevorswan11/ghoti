#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

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

} // namespace ghoti::tests
