#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

// §10.1's compositional construction builtins: the inverse of `@typeInfo` for scalar kinds.
TEST_CASE("`@Int` constructs an integer type from an `IntInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Int(builtin.IntInfo{ .bits = 32, .signed = true });
            var a: T = 42;
            return a;
        };
    )") == 42);
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Int(builtin.IntInfo{ .bits = 8, .signed = false });
            var a: T = 200;
            return @intCast(i32, a);
        };
    )") == 200);
}

TEST_CASE("`@Float` constructs a floating-point type from a `FloatInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Float(builtin.FloatInfo{ .bits = 64 });
            var b: T = 3.5;
            return if (b == 3.5) 1 else 0;
        };
    )") == 1);
}

TEST_CASE("`@Float` rejects a bit width with no matching floating-point type") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const T := @Float(builtin.FloatInfo{ .bits = 24 });
            var b: T = 0.0;
            return 0;
        };
    )");
}

TEST_CASE("`@Pointer` constructs a pointer type from a `PointerInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Pointer(builtin.PointerInfo{ .child = i32, .is_mut = false, .is_volatile = false });
            var v: i32 = 7;
            var p: T = ^v;
            return *p;
        };
    )") == 7);
}

TEST_CASE("`@Reference` constructs a reference type from a `PointerInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Reference(builtin.PointerInfo{ .child = i32, .is_mut = true, .is_volatile = false });
            var w: i32 = 3;
            var r: T = &mut w;
            r = 11;
            return w;
        };
    )") == 11);
}

TEST_CASE("`@Slice` constructs a slice type from a `SliceInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Slice(builtin.SliceInfo{ .child = i32, .sentinel = false, .is_mut = false, .is_volatile = false });
            var arr: [3]i32 = .{1, 2, 3};
            var s: T = arr[0..3];
            return @intCast(i32, s.len);
        };
    )") == 3);
}

TEST_CASE("`@Array` constructs an array type from an `ArrayInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Array(builtin.ArrayInfo{ .child = i32, .len = 4, .sentinel = false, .is_mut = false, .is_volatile = false });
            var ar: T = .{9, 9, 9, 9};
            return ar[0];
        };
    )") == 9);
}

// Regression: constructing a `PointerInfo`/`SliceInfo`/`ArrayInfo` descriptor via struct-literal
// syntax (every one of them carries a `child: type` field) used to crash LLVM outright
// (`Ty->isSized()` assertion trying to lay out a `type`-kind struct field for real runtime
// storage) - the same root cause as §11's "aliased void" finding. Fixed by never physically
// storing into a `type`-kind field (it's a compile-time-only, zero-sized placeholder purely so
// later fields' GEP indices stay correct) rather than trying to give it a real value.
TEST_CASE("a struct literal with a `type`-kind field constructs without crashing") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const desc := builtin.PointerInfo{ .child = i32, .is_mut = false, .is_volatile = false };
            return if (desc.is_mut) 1 else 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
