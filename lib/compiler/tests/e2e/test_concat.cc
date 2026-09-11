#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`++` concatenates two byte-string literals") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr combined := "ab" ++ "cde";
            if (combined[0] != 'a') { return 1; }
            if (combined[1] != 'b') { return 2; }
            if (combined[2] != 'c') { return 3; }
            if (combined[3] != 'd') { return 4; }
            if (combined[4] != 'e') { return 5; }
            return 0;
        };
    )") == 0);
}

// Module-scope constants: today only these (and literals) fold as `++` operands. A local
// array/slice identifier's *value* cannot yet be const-evaluated by name (see test below).
TEST_CASE("`++` concatenates two constexpr integer arrays") {
    CHECK(helpers::compile_and_run(R"(
        const a: [2]i32 = .{1, 2};
        const b: [3]i32 = .{3, 4, 5};
        pub const main := fn(): i32 {
            constexpr combined := a ++ b;
            return combined[0] + combined[1] + combined[2] + combined[3] + combined[4];
        };
    )") == 15);
}

TEST_CASE("`++` chains left to right") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            constexpr combined := "a" ++ "b" ++ "c";
            if (combined[0] != 'a') { return 1; }
            if (combined[1] != 'b') { return 2; }
            if (combined[2] != 'c') { return 3; }
            return 0;
        };
    )") == 0);
}

TEST_CASE("`++` takes its sentinel from the right operand only") {
    CHECK(helpers::compile_and_run(R"(
        const plain: [2]u8 = .{1, 2};
        pub const main := fn(): i32 {
            constexpr rhs_has_sentinel := plain ++ "cd";
            if (@bitSizeOf(@typeOf(rhs_has_sentinel)) != @bitSizeOf([4:0]u8)) { return 1; }
            constexpr rhs_has_none := "ab" ++ plain;
            if (@bitSizeOf(@typeOf(rhs_has_none)) != @bitSizeOf([4]u8)) { return 2; }
            return 0;
        };
    )") == 0);
}

// A local array binding's own value cannot yet be looked up by name at const-eval time (only
// module-scope constants and literals can); `++` reports this cleanly rather than emitting it.
TEST_CASE("`++` on a local array identifier is a clean compile error, not a crash") {
    helpers::expect_compile_error(R"(
        pub const main := fn(): i32 {
            const a: [2]i32 = .{1, 2};
            constexpr combined := a ++ a;
            return combined[0];
        };
    )");
}

} // namespace ghoti::tests
