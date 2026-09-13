#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`expr...` forwards a pack to another pack function") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(rest...): i32 {
            var total: i32 = 0;
            for constexpr (rest) |v| { total = total + v; }
            return total;
        };
        const forward_only := fn(rest...): i32 { return sum(rest...); };
        pub const main := fn(): i32 { return forward_only(5, 6, 7); };
    )") == 18);
}

TEST_CASE("`expr...` forwards a pack alongside a leading fixed argument") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(rest...): i32 {
            var total: i32 = 0;
            for constexpr (rest) |v| { total = total + v; }
            return total;
        };
        const forward := fn(a: i32, rest...): i32 { return a + sum(rest...); };
        pub const main := fn(): i32 { return forward(1, 2, 3, 4); };
    )") == 10);
}

TEST_CASE("the same pack may be expanded more than once in one call site") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(rest...): i32 {
            var total: i32 = 0;
            for constexpr (rest) |v| { total = total + v; }
            return total;
        };
        const twice := fn(rest...): i32 { return sum(rest...) + sum(rest...); };
        pub const main := fn(): i32 { return twice(1, 2); };
    )") == 6);
}

TEST_CASE("`expr...` forwards a pack into a plain (non-pack) function's fixed parameters") {
    CHECK(helpers::compile_and_run(R"(
        const add2 := fn(a: i32, b: i32): i32 { return a + b; };
        const call_add2 := fn(rest...): i32 { return add2(rest...); };
        pub const main := fn(): i32 { return call_add2(3, 4); };
    )") == 7);
}

TEST_CASE("the same pack function instantiated at two different arities does not corrupt shared "
          "`for constexpr` scope typing") {
    CHECK(helpers::compile_and_run(R"(
        const sum := fn(rest...): i32 {
            var total: i32 = 0;
            for constexpr (rest) |v| { total = total + v; }
            return total;
        };
        pub const main := fn(): i32 { return sum(1, 2, 3, 4) + sum(1, 2); };
    )") == 13);
}

TEST_CASE("`expr...` may only expand the enclosing parameter pack") {
    helpers::expect_compile_error(R"(
        const sum := fn(rest...): i32 { return rest.len; };
        const bad := fn(rest...): i32 {
            var arr: [2]i32 = .{1, 2};
            return sum(arr...);
        };
        pub const main := fn(): i32 { return bad(1, 2); };
    )");
}

TEST_CASE("a pack expansion's spliced arity is still checked against the callee") {
    helpers::expect_compile_error(R"(
        const add2 := fn(a: i32, b: i32): i32 { return a + b; };
        const call_add2 := fn(rest...): i32 { return add2(rest...); };
        pub const main := fn(): i32 { return call_add2(3, 4, 5); };
    )");
}

} // namespace ghoti::tests
