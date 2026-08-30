#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a positional `Alias{ ... }` literal initializes an array-type alias") {
    CHECK(helpers::compile_and_run(R"(
        const Row := [3]i32;
        pub const main := fn(): i32 {
            const r := Row{ 10, 20, 12 };
            return r[0] + r[1] + r[2];
        };
    )") == 42);
}

TEST_CASE("an implicit `.{ ... }` literal initializes an array from context") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const r: [4]i32 = .{ 1, 2, 3, 4 };
            return r[0] + r[1] + r[2] + r[3];
        };
    )") == 10);
}

TEST_CASE("an array-alias literal is usable as a call argument") {
    CHECK(helpers::compile_and_run(R"(
        const Triple := [3]i32;
        const sum := fn(t: Triple): i32 { return t[0] + t[1] + t[2]; };
        pub const main := fn(): i32 {
            return sum(Triple{ 15, 15, 12 });
        };
    )") == 42);
}

TEST_CASE("an array-alias literal with the wrong element count is rejected") {
    helpers::expect_compile_error(R"(
        const Row := [3]i32;
        pub const main := fn(): i32 {
            const r := Row{ 1, 2 };
            return r[0];
        };
    )");
}

} // namespace ghoti::tests
