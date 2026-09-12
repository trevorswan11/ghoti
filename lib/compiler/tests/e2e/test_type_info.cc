#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

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

TEST_CASE("the full `builtin.TypeInfo` union (all 16 arms) resolves cleanly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const T: type = builtin.TypeInfo;
            _ = T;
            return 0;
        };
    )") == 0);
}

} // namespace ghoti::tests
