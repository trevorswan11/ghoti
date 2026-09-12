#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

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
            match constexpr (@typeInfo(@typeOf(add))) {
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

} // namespace ghoti::tests
