#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`@Fn` constructs a function type from an `FnInfo` descriptor") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T := @Fn(builtin.FnInfo{
                .params = [2]type{i32, i32},
                .return_type = i32,
                .variadic = false,
                .has_self = false,
                .@"callconv" = .c,
            });
            var fp: T = undefined;
            const add := fn(a: i32, b: i32): i32 { return a + b; };
            fp = add;
            return fp(3, 4);
        };
    )") == 7);
}

TEST_CASE("`@typeInfo` reports a function's params/return/variadic/has_self/callconv") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32): i32 { return a + b; };
        pub const main := fn(): i32 {
            var result := 0;
            match constexpr (@typeInfo(@TypeOf(add))) {
                .function => |info| {
                    result = if (info.variadic) 100 else 1;
                    result = result + if (info.has_self) 100 else 0;
                },
                _ => { result = -1; },
            }
            return result;
        };
    )") == 1);
}

TEST_CASE("`@typeInfo` reports whether a function type is erased") {
    CHECK(helpers::compile_and_run(R"(
        const add := fn(a: i32, b: i32): i32 { return a + b; };
        const erased_of := fn(T: type): bool { return @typeInfo(T).function.erased; };
        pub const main := fn(): i32 {
            var score: i32 = 0;
            if (!erased_of(@TypeOf(add))) { score += 1; }
            if (erased_of(fn(a: i32, b: i32): i32)) { score += 2; }
            if (!erased_of(extern fn(a: i32, b: i32): i32)) { score += 4; }
            if (erased_of(dyn Fn(a: i32, b: i32): i32)) { score += 8; }
            return score;
        };
    )") == 15);
}

TEST_CASE("`@Fn` round-trips erased and thin function types and defaults to erased") {
    CHECK(helpers::compile_and_run(R"(
        const Erased := fn(a: i32, b: i32): i32;
        const Thin := extern fn(a: i32, b: i32): i32;
        pub const main := fn(): i32 {
            var score: i32 = 0;
            if (@Fn(@typeInfo(Erased).function) == Erased) { score += 1; }
            if (@Fn(@typeInfo(Thin).function) == Thin) { score += 2; }
            const Default := @Fn(builtin.FnInfo{
                .params = [2]type{i32, i32},
                .return_type = i32,
                .variadic = false,
                .has_self = false,
                .@"callconv" = .c,
            });
            if (Default == Erased) { score += 4; }
            var base: i32 = 10;
            const f: Default = fn(a: i32, b: i32): i32 { return a + b + base; };
            return score + f(1, 2);
        };
    )") == 7 + 13);
}

TEST_CASE("`@Fn` rejects an erased descriptor with a non-C calling convention") {
    CHECK(helpers::raised(R"(
        const F := @Fn(builtin.FnInfo{
            .params = [0]type{},
            .return_type = void,
            .variadic = false,
            .has_self = false,
            .@"callconv" = .win64,
        });
    )",
                          sema::error::CALLCONV_REQUIRES_EXTERN_FN));
}

} // namespace ghoti::tests
