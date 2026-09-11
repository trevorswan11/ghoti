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

// `@Pointer`/`@Reference`/`@Slice`/`@Array` are implemented at the sema level (fold their
// `PointerInfo`/`SliceInfo`/`ArrayInfo` descriptor and construct the matching type - see
// `docs/comptime-metaprogramming-plan.md` §10.1) but are NOT exercised by a test here: any of
// those descriptors has a `child: type` field, and constructing one via ordinary struct-literal
// syntax hits a pre-existing gap where LLVM *aborts the process* (`Ty->isSized()` assertion, not a
// clean diagnostic) trying to lay out a `type`-kind struct field for real runtime storage - the
// same class of issue noted in §11's "aliased void" finding. A crashing scenario cannot be a
// `helpers::expect_compile_error` test (that would abort the whole suite); fixing it for real
// means giving `type`-kind fields a real (degenerate) sized LLVM representation in
// `type_translator`/`llvm_lowering`, a separate unit of work - see §10.1's status note.

} // namespace ghoti::tests
