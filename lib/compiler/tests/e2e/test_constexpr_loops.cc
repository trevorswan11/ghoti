#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

// Phase 3 only wires the syntax through; `for`/`while constexpr` still run as ordinary loops
// until unrolling lands (Part I §5/§6 of the design). These just confirm nothing crashes.
// The `else`/labeled rejections are pure parse errors; see tests/ast/errors/test_pack_params.cc.
TEST_CASE("`for constexpr` parses and runs (not yet unrolled)") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var sum := 0;
            for constexpr (0..3) |v| { sum = sum + v; }
            return sum;
        };
    )") == 3);
}

TEST_CASE("`while constexpr` parses and runs (not yet unrolled)") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var n := 0;
            while constexpr (n < 3) { n = n + 1; }
            return n;
        };
    )") == 3);
}

} // namespace ghoti::tests
