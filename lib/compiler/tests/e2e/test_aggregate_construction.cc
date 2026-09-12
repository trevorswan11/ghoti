#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`@Enum` constructs an enum type from an `EnumInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Enum(builtin.EnumInfo{
                .tag_type = i32,
                .fields = [2]builtin.EnumField{
                    .{ .name = "a", .value = 10 },
                    .{ .name = "b", .value = 20 },
                },
                .exhaustive = true,
            });
            var v: T = T.b;
            return @as(i32, v);
        };
    )") == 20);
}

TEST_CASE("`@Enum` variants are usable with `@tagName`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Enum(builtin.EnumInfo{
                .tag_type = i32,
                .fields = [2]builtin.EnumField{
                    .{ .name = "a", .value = 10 },
                    .{ .name = "b", .value = 20 },
                },
                .exhaustive = true,
            });
            const s := @tagName(T.b);
            return @intCast(i32, s.len) + @as(i32, s[0]);
        };
    )") == 99);
}

TEST_CASE("`@Struct` constructs a struct type from a `StructInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.FieldInfo{
                    .{ .name = "x", .type_ = i32, .has_default = false },
                    .{ .name = "y", .type_ = i32, .has_default = false },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 10, .y = 20 };
            return v.x + v.y;
        };
    )") == 30);
}

TEST_CASE("`@Struct` applies `defaults...` to fields with `has_default = true`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.FieldInfo{
                    .{ .name = "x", .type_ = i32, .has_default = false },
                    .{ .name = "y", .type_ = i32, .has_default = true },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            }, 99);
            var v: T = .{ .x = 1 };
            return v.x + v.y;
        };
    )") == 100);
}

TEST_CASE("`@Struct` diagnoses a missing `defaults...` argument instead of crashing") {
    CHECK(helpers::raised(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [1]builtin.FieldInfo{
                    .{ .name = "x", .type_ = i32, .has_default = true },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{};
            return v.x;
        };
    )",
                          sema::error::ARITY_MISMATCH));
}

TEST_CASE("`@Union` constructs an untagged union type from a `UnionInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Union(builtin.UnionInfo{
                .fields = [2]builtin.FieldInfo{
                    .{ .name = "i", .type_ = i32, .has_default = false },
                    .{ .name = "f", .type_ = f32, .has_default = false },
                },
                .is_extern = false,
                .is_packed = false,
                .tagged = false,
            });
            var v: T = .{ .i = 42 };
            return v.i;
        };
    )") == 42);
}

TEST_CASE("`@Union` constructs a tagged union type from a `UnionInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Union(builtin.UnionInfo{
                .fields = [2]builtin.FieldInfo{
                    .{ .name = "i", .type_ = i32, .has_default = false },
                    .{ .name = "f", .type_ = f32, .has_default = false },
                },
                .is_extern = false,
                .is_packed = false,
                .tagged = true,
            });
            var v: T = .{ .i = 55 };
            return v.i;
        };
    )") == 55);
}

} // namespace ghoti::tests
