#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using helpers::mock_file;

TEST_CASE("`@Enum` constructs an enum type from an `EnumInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Enum(builtin.EnumInfo{
                .tag_type = i32,
                .fields = [2]builtin.EnumFieldInfo{
                    .{ .name = "a", .value = 10 },
                    .{ .name = "b", .value = 20 },
                },
                .exhaustive = true,
            });
            var v: T = T.b;
            return @backingInt(v);
        };
    )") == 20);
}

TEST_CASE("`@Enum` variants are usable with `@tagName`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Enum(builtin.EnumInfo{
                .tag_type = i32,
                .fields = [2]builtin.EnumFieldInfo{
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
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "y", .@"type" = i32 },
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

TEST_CASE("`@Struct` accepts a `^.{...}` slice literal for its `fields` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(.{
                .fields = ^.{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "y", .@"type" = i32 },
                    .{ .name = "z", .@"type" = i32, .default_value = @ptrCast(^opaque, ^1) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 2, .y = 9 };
            return v.x + v.y + v.z;
        };
    )") == 12);
}

TEST_CASE("`@Struct` applies a field's own `default_value`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "y", .@"type" = i32, .default_value = @ptrCast(^opaque, ^99) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 1 };
            return v.x + v.y;
        };
    )") == 100);
}

TEST_CASE("`@Struct`'s `default_value` accepts `^<module-scope const>`") {
    CHECK(helpers::compile_and_run(R"(
        const DEFAULT_Y: i32 = 99;
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "y", .@"type" = i32, .default_value = @ptrCast(^opaque, ^DEFAULT_Y) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 1 };
            return v.x + v.y;
        };
    )") == 100);
}

TEST_CASE("`@Struct`'s `default_value` accepts `^<local const>`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const local_default: i32 = 99;
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "y",
                       .@"type" = i32,
                       .default_value = @ptrCast(^opaque, ^local_default) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 1 };
            return v.x + v.y;
        };
    )") == 100);
}

TEST_CASE("`@Struct`'s `default_value` accepts `^<constexpr parameter>` inside a generic "
          "`fn(...): type` constructor") {
    CHECK(helpers::compile_and_run(R"(
        const Point := fn(T: type, constexpr default_z: T): type {
            return @Struct(.{
                .fields = ^.{
                    .{ .name = "x", .@"type" = T },
                    .{ .name = "y", .@"type" = T },
                    .{ .name = "z", .@"type" = T, .default_value = @ptrCast(^opaque, ^default_z) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
        };

        pub const main := fn(): i32 {
            const p: Point(i32, 1) = .{ .x = 2, .y = 9 };
            return p.x + p.y + p.z;
        };
    )") == 12);
}

TEST_CASE("`@Struct`'s `default_value` accepts a struct-typed value") {
    CHECK(helpers::compile_and_run(R"(
        const Point := struct { a: i32, b: i32 };
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                    .{ .name = "pt",
                       .@"type" = Point,
                       .default_value = @ptrCast(^opaque, ^Point{ .a = 3, .b = 4 }) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 1 };
            return v.x + v.pt.a + v.pt.b;
        };
    )") == 8);
}

TEST_CASE("`@Struct`'s `default_value` accepts an array-typed value") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [1]builtin.StructFieldInfo{
                    .{ .name = "arr",
                       .@"type" = [2]i32,
                       .default_value = @ptrCast(^opaque, ^[2]i32{ 5, 6 }) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{};
            return v.arr[0] + v.arr[1];
        };
    )") == 11);
}

TEST_CASE("`@Struct`'s `default_value` recurses through a nested aggregate") {
    CHECK(helpers::compile_and_run(R"(
        const Inner := struct { n: i32 };
        const Outer := struct { inner: Inner, tag: i32 };
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [1]builtin.StructFieldInfo{
                    .{ .name = "o",
                       .@"type" = Outer,
                       .default_value = @ptrCast(
                           ^opaque, ^Outer{ .inner = .{ .n = 7 }, .tag = 2 }) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{};
            return v.o.inner.n + v.o.tag;
        };
    )") == 9);
}

TEST_CASE("`@Struct`'s `default_value` accepts `^<cross-module const>`") {
    constexpr std::string_view defaults_gh{R"(
        pub const DEFAULT_Y: i32 = 99;
    )"};

    CHECK(helpers::compile_and_run(
              R"(
            import "defaults.gh" as defaults;
            pub const main := fn(): i32 {
                const T := @Struct(builtin.StructInfo{
                    .fields = [2]builtin.StructFieldInfo{
                        .{ .name = "x", .@"type" = i32 },
                        .{ .name = "y",
                           .@"type" = i32,
                           .default_value = @ptrCast(^opaque, ^defaults.DEFAULT_Y) },
                    },
                    .is_extern = false,
                    .is_packed = false,
                    .backing_bits = 0,
                });
                var v: T = .{ .x = 1 };
                return v.x + v.y;
            };
        )",
              {mock_file{"defaults.gh", defaults_gh, "defaults"}}) == 100);
}

TEST_CASE("`@Union` constructs an untagged union type from a `UnionInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Union(builtin.UnionInfo{
                .fields = [2]builtin.UnionFieldInfo{
                    .{ .name = "i", .@"type" = i32 },
                    .{ .name = "f", .@"type" = f32 },
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

TEST_CASE("`@Struct` widens an unsuffixed float `default_value` to a narrower field's own "
          "type instead of defaulting it to `f64`") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [2]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = f32 },
                    .{ .name = "y", .@"type" = f32, .default_value = @ptrCast(^opaque, ^1.0f32) },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 2f32 };
            return @as(i32, v.x + v.y);
        };
    )") == 3);
}

TEST_CASE("`@Struct` diagnoses a field descriptor with a typo'd key instead of crashing") {
    CHECK(helpers::raised(R"(
        pub const main := fn(): i32 {
            const T := @Struct(builtin.StructInfo{
                .fields = [1]builtin.StructFieldInfo{
                    .{ .name = "x", .type_ = i32 },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            var v: T = .{ .x = 1 };
            return v.x;
        };
    )",
                          sema::error::MISSING_FIELD));
}

TEST_CASE("`@Struct`/`@Union`/`@Enum` infer an implicit `.{...}` descriptor's type") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const S := @Struct(.{
                .fields = [1]builtin.StructFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                },
                .is_extern = false,
                .is_packed = false,
                .backing_bits = 0,
            });
            const U := @Union(.{
                .fields = [1]builtin.UnionFieldInfo{
                    .{ .name = "x", .@"type" = i32 },
                },
                .is_extern = false,
                .is_packed = false,
                .tagged = false,
            });
            const E := @Enum(.{
                .tag_type = i32,
                .fields = [1]builtin.EnumFieldInfo{ .{ .name = "a", .value = 5 } },
                .exhaustive = true,
            });
            var s: S = .{ .x = 1 };
            var u: U = .{ .x = 2 };
            var e: E = E.a;
            return s.x + u.x + @backingInt(e);
        };
    )") == 8);
}

TEST_CASE("`@Union` constructs a tagged union type from a `UnionInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Union(builtin.UnionInfo{
                .fields = [2]builtin.UnionFieldInfo{
                    .{ .name = "i", .@"type" = i32 },
                    .{ .name = "f", .@"type" = f32 },
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
