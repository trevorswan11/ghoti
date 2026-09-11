#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// Phase 8: `builtin.gh.inc` gained `TypeKind`/`CallConv`/`*Info`/`TypeInfo` (Part III's Layer 2
// shapes, no C++) - `@typeInfo` itself, which actually *constructs* a `TypeInfo` value, lands in
// Phase 9. A `type`-typed struct field (`PointerInfo.child`, `FnInfo.return_type`, ...) cannot
// yet be assigned a concrete type value ("Type mismatch in store: cannot assign 'i32' to
// 'type'") - unlike a `T: type` *generic parameter*, an ordinary aggregate field declared `type`
// has no runtime representation built for it yet. That in turn means `TypeInfo` itself (a union
// whose layout must size every arm, including the `type`-bearing ones) cannot be constructed for
// *any* arm, not just the ones with the problem field. Phase 9 needs to close this gap before
// `@typeInfo`/`@field`/etc. can return real values - flagged here rather than worked around,
// since fixing it means relaxing `type_checker.cc`'s field-assignment compatibility for
// `type`-kind fields, which is squarely that phase's "resolver + const_eval" work, not this
// phase's "no C++" one. These tests cover what Phase 8 shipped: the shapes resolve cleanly (the
// full `TypeInfo` union already type-checks with zero diagnostics - see `inject_builtin_module`'s
// `VERIFY`) and every construct that has *no* `type`-typed field works today.
TEST_CASE("`builtin.TypeKind` values construct and match") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const k: builtin.TypeKind = .int;
            return match (k) {
                .int => 1,
                _ => 0,
            };
        };
    )") == 1);
}

TEST_CASE("`builtin.CallConv` matches `ast::calling_convention`'s spellings") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const cc: builtin.CallConv = .c;
            return match (cc) {
                .c => 1,
                .sysv, .win64, .stdcall, .fastcall, .aapcs => 0,
            };
        };
    )") == 1);
}

TEST_CASE("`builtin.IntInfo` carries a bit width and signedness") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const info: builtin.IntInfo = .{ .bits = 32, .signed = true };
            return @intCast(i32, info.bits);
        };
    )") == 32);
}

TEST_CASE("`builtin.FloatInfo` carries a bit width") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const info: builtin.FloatInfo = .{ .bits = 64 };
            return @intCast(i32, info.bits);
        };
    )") == 64);
}

// The regression this guards: `TypeInfo`'s arms are named after `TypeKind` (`bool`, `void`,
// `noreturn`, ...), and several of those arms are themselves payload-less. Resolving a bare
// `void` field type from *inside* the union's own body found the union's own `void`-named arm (a
// value symbol) ahead of the builtin `void` type in scope, poisoning silently (no diagnostic) - a
// real pre-existing scoping bug the raw-identifier convention for reserved-word member names
// exposed for the first time. An aliased `void` sidestepped the name collision but not a second,
// separate issue - LLVM's `void` has no size, and a codegen path reached through the alias
// crashed trying to size it, so the payload-less arms use a zero-sized empty struct
// (`builtin.gh.inc`'s `NoPayload`) instead. This confirms the whole 16-arm union resolves; see
// the file-level comment above for why no arm can be *constructed* yet.
TEST_CASE("the full `builtin.TypeInfo` union (all 16 arms) resolves cleanly") {
    // `helpers::compile_and_run` would try to construct a value and hit the `type`-field gap;
    // referencing the type by name alone is enough to prove the union itself type-checks.
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T: type = builtin.TypeInfo;
            _ = T;
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
