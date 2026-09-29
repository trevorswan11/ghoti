#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("@assert / @verify accept a runtime bool condition and an optional message") {
    helpers::resolve_and_check(R"(
const use := fn(x: i32): void {
    @assert(x == 0);
    @assert(x == 0, "x should be zero");
    @verify(x > -1);
    @verify(x > -1, "x should be in range");
};
)");
}

TEST_CASE("a constexpr-known-false @assert is a compile error") {
    CHECK(helpers::raised("const use := fn(): void { @assert(1 == 2); };",
                          sema::error::STATIC_ASSERTION_FAILED));
}

TEST_CASE("a constexpr-known-false @verify is a compile error") {
    CHECK(helpers::raised("const use := fn(): void { @verify(false, \"nope\"); };",
                          sema::error::STATIC_ASSERTION_FAILED));
}

TEST_CASE("a constexpr-known-true @assert / @verify raises nothing") {
    CHECK(helpers::resolver_error_codes(
              "const use := fn(): void { @assert(1 == 1); @verify(2 > 1); };")
              .empty());
}

TEST_CASE("@assert / @verify accept a bool or pointer condition") {
    helpers::resolve_and_check(R"(
const use := fn(p: ^i32): void {
    @assert(p);
    @assert(p, "must be non-null");
    @verify(p);
    @verify(p, "must be non-null");
};
)");
}

TEST_CASE("@assert / @verify reject a non-bool, non-pointer condition") {
    CHECK(helpers::raised("const use := fn(x: f64): void { @assert(x); };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const use := fn(n: i32): void { @verify(n); };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("@expect / @require accept a bool or a pointer condition (pointer means `!= null`)") {
    helpers::resolve_and_check(R"(
test "conditions" {
    var x: i32 = 0;
    const p: ^i32 = ^x;
    @expect(x == 0);
    @require(x == 0, "x should be zero");
    @require(p != nullptr);
    @expect(p);              // contextually converted like `if (p)`
    @require(p, "must be non-null");
}
)");
}

TEST_CASE("@expect / @require still reject a non-bool, non-pointer condition") {
    CHECK(helpers::raised(R"(test "t" { var n: i32 = 1; @require(n, "nonzero"); })",
                          sema::error::TYPE_MISMATCH));
    CHECK(
        helpers::raised(R"(test "t" { var n: i32 = 1; @expect(n); })", sema::error::TYPE_MISMATCH));
}

TEST_CASE("@assert / @verify reject the wrong argument count") {
    CHECK(helpers::raised("const use := fn(): void { @assert(); };", sema::error::ARITY_MISMATCH));
    CHECK(helpers::raised("const use := fn(x: i32): void { @verify(x > 0, \"a\", \"b\"); };",
                          sema::error::ARITY_MISMATCH));
}

TEST_CASE("A failing @assert in an arm a folded condition rules out is not an error") {
    helpers::resolve_and_check(R"(
        constexpr N := 2;
        constexpr {
            if (N > 4) { @assert(N > 4); } else { @assert(N <= 4); }
        }
        const f := fn(constexpr M: usize): usize {
            constexpr {
                if (M > 4) { @assert(M > 4); } else { @assert(M <= 4); }
            }
            match (M) {
                2 => @assert(M == 2),
                _ => @assert(M != 2),
            }
            return M;
        };
        pub const main := fn(): i32 {
            if (N > 4) { @assert(N > 4); }
            const a := f(2);
            const b := f(8);
            return 0;
        };
    )");
}

TEST_CASE("A failing @assert in the live arm is still an error") {
    CHECK(helpers::raised(R"(
        const f := fn(constexpr M: usize): usize {
            if (M > 4) { @assert(M < 4); }
            return M;
        };
        pub const main := fn(): i32 {
            const b := f(8);
            return 0;
        };
    )",
                          sema::error::STATIC_ASSERTION_FAILED));
    CHECK(helpers::raised(R"(
        constexpr N := 2;
        pub const main := fn(): i32 {
            match (N) {
                2 => @assert(N == 3),
                _ => {},
            }
            return 0;
        };
    )",
                          sema::error::STATIC_ASSERTION_FAILED));
}

} // namespace ghoti::tests
