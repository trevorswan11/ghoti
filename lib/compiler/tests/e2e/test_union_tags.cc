#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "compiler/syntax/error.hh"
#include "helpers/ast.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("A union must name its tag") {
    helpers::test_parser_fail(
        "const U = union { a: i32 };",
        syntax::diagnostic{"A union names its tag: `union(enum)` for a tagged union, "
                           "`union(MyEnum)` to tag it with an enum, `union(void)` for an untagged "
                           "one, or `extern union` for the C layout",
                           syntax::error::MISSING_UNION_TAG,
                           0,
                           10});
    helpers::test_parser_fail(
        "const U = extern union(enum) { a: i32 };",
        syntax::diagnostic{"An `extern` union has no tag, so it cannot name one",
                           syntax::error::ILLEGAL_UNION_TAG,
                           0,
                           22});
    helpers::test_parser_fail(
        "const U = packed union(void) { a: u8 };",
        syntax::diagnostic{"A `packed` union has no tag, so it cannot name one",
                           syntax::error::ILLEGAL_UNION_TAG,
                           0,
                           22});
}

TEST_CASE("A union tagged by an enum reads as that enum") {
    CHECK(helpers::compile_and_run(R"(
        const K = enum : u8 { a = 3, b = 7, c = 9 };
        const U = union(K) { c: bool, a: i32, b: u8 };
        pub const main = fn(): i32 {
            let u: U = .{ .b = 2 };
            let k: K = u;
            const folded: K = U{ .c = true };
            const payload = match (u) { .b => |x| @intCast(i32, x), _ => 0 };
            return if (k == .b and folded == .c and payload == 2) 0 else 1;
        };
    )") == 0);

    // An inline enum works as the tag too
    CHECK(helpers::compile_and_run(R"(
        const U = union(enum { x, y }) { x: i32, y: u8 };
        pub const main = fn(): i32 {
            let u: U = .{ .y = 4 };
            return match (u) { .x => |v| v, .y => |v| @intCast(i32, v) };
        };
    )") == 4);
}

TEST_CASE("A union may be tagged by a non-exhaustive enum") {
    CHECK(helpers::compile_and_run(R"(
        const K = enum : u8 { a, b, _ };
        const U = union(K) { a: i32, b: u8 };
        pub const main = fn(): i32 {
            let mut u: U = .{ .a = 1 };
            u = .{ .b = 3 };
            let k: K = u;
            // The union only holds named variants, so its fields cover a `match`
            return match (u) { .a => |v| v, .b => |v| @intCast(i32, v) + @intCast(i32, @backingInt(k)) };
        };
    )") == 4);
}

TEST_CASE("A union's tag enum must match its fields") {
    CHECK(helpers::raised("const K = enum { a, b }; const U = union(K) { a: i32 };",
                          sema::error::ILLEGAL_UNION_TAG));
    CHECK(helpers::raised("const K = enum { a, b }; const U = union(K) { a: i32, b: u8, c: bool };",
                          sema::error::ILLEGAL_UNION_TAG));
    CHECK(helpers::raised(R"(
        const U = union(i32) { a: i32 };
        pub const main = fn(): i32 { let u: U = .{ .a = 1 }; return 0; };
    )",
                          sema::error::ILLEGAL_UNION_TAG));
}

TEST_CASE("An untagged union keeps Ghoti's layout and reinterprets its bytes") {
    CHECK(helpers::compile_and_run(R"(
        const B = union(void) { all: u64, halves: [2]u32 };
        pub const main = fn(): i32 {
            let b: B = .{ .all = 0x0000000500000003 };
            return @intCast(b.halves[0] + b.halves[1]);
        };
    )") == 8);
    CHECK(helpers::raised(R"(
        const B = union(void) { a: i32, b: u8 };
        pub const main = fn(): i32 { let v: B = .{ .a = 1 }; return match (v) { .a => 0, .b => 1 }; };
    )",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("@typeInfo and @Union carry a union's tag enum") {
    CHECK(helpers::compile_and_run(R"(
        const K = enum { a, b };
        const U = union(K) { b: u8, a: i32 };
        const V = union(enum) { a: i32 };
        const W = @Union(.{
            .fields = @typeInfo(U).@"union".fields,
            .is_extern = false,
            .is_packed = false,
            .tagged = true,
            .tag_type = K,
        });
        pub const main = fn(): i32 {
            if comptime (@typeInfo(U).@"union".tag_type != K) { return 1; }
            if comptime (@typeInfo(V).@"union".tag_type != void) { return 2; }
            let w: W = .{ .a = 5 };
            let k: K = w;
            return if (k == .a) 0 else 3;
        };
    )") == 0);
}

} // namespace ghoti::tests
