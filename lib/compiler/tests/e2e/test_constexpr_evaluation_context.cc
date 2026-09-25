#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("a constexpr block folds calls into functions that take a parameter pack") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr max := fn(a: auto, b: auto, c...): auto {
            var largest := if (a > b) a else b;
            for constexpr (c) |arg| {
                if (arg > largest) largest = arg;
            }
            return largest;
        };
        pub constexpr count := fn(c...): usize { return c.len; };
        pub constexpr second := fn(c...): auto { return c[1]; };
        pub constexpr forward := fn(a: auto, c...): auto { return max(a, a, c...); };

        test "max" {
            @expect(max(-4, -123, 45, 78, 90, -23) == 90);
            constexpr {
                @assert(max(1, 2) == 2);
                @assert(max(-4, -123, 45) == 45);
                @assert(count() == 0);
                @assert(count(1, 2, 3) == 3);
                @assert(second(7, 8, 9) == 8);
                @assert(forward(4, 9, 1) == 9);
            }
        }
    )") == 0);
}

TEST_CASE("a false compile-time assertion over a pack call is reported") {
    helpers::expect_compile_error(R"(
        pub constexpr count := fn(c...): usize { return c.len; };
        test "t" { constexpr { @assert(count(1, 2) == 3); } }
    )");
}

TEST_CASE("a condition-less `if constexpr` picks its arm from the evaluation context") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr where := fn(): i32 { return if constexpr 1 else 2; };
        pub constexpr tally := fn(): i32 {
            var n: i32 = 10;
            if constexpr { n += 5; }
            return n;
        };
        constexpr AT_COMPILE := where();

        test "context" {
            constexpr {
                @assert(where() == 1);
                @assert(tally() == 15);
            }
            constexpr in_label: {
                @assert(where() == 1);
            }
            @expect(AT_COMPILE == 1);
            var at_runtime := where();
            @expect(at_runtime == 2);
            var runtime_tally := tally();
            @expect(runtime_tally == 10);
        }
    )") == 0);
}

TEST_CASE("both arms of a condition-less `if constexpr` are type checked") {
    helpers::expect_compile_error(R"(
        const f := fn(): i32 { return if constexpr 1 else true; };
        pub const main := fn(): i32 { return f(); };
    )");
}

TEST_CASE("a constexpr function's params read at compile time are implicitly constexpr") {
    CHECK(helpers::compile_and_run_tests(R"(
        pub constexpr min_cx := fn(a: i32, b: i32): i32 {
            return if constexpr (a < b) a else b;
        };
        pub constexpr pick := fn(a: auto, b: auto): auto {
            constexpr smaller := if (a < b) a else b;
            return smaller;
        };
        pub constexpr size_of := fn(x: auto): usize {
            constexpr size := @sizeOf(@TypeOf(x));
            return size;
        };

        test "min_cx" {
            constexpr {
                @assert(min_cx(1, 2) == 1);
                @assert(min_cx(-4, -123) == -123);
                @assert(pick(9, 3) == 3);
            }
            @expect(min_cx(3, 4) == 3);
            var runtime: i64 = 5;
            @expect(size_of(runtime) == 8);
        }
    )") == 0);
}

TEST_CASE("a runtime argument to an implicitly constexpr parameter says why it is constexpr") {
    CHECK(helpers::raised(R"(
        pub constexpr min_cx := fn(a: i32, b: i32): i32 {
            return if constexpr (a < b) a else b;
        };
        const run := fn(x: i32): i32 { return min_cx(x, 4); };
    )",
                          sema::error::CONSTEXPR_EVALUATION_FAILED));
}

TEST_CASE("params of a non-constexpr function are never inferred constexpr") {
    helpers::expect_compile_error(R"(
        const min_rt := fn(a: i32, b: i32): i32 {
            return if constexpr (a < b) a else b;
        };
        pub const main := fn(): i32 { return min_rt(1, 2); };
    )");
}

} // namespace ghoti::tests
