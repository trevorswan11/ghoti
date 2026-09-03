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

TEST_CASE("a comptime-known-false @assert is a compile error") {
    CHECK(helpers::raised("const use := fn(): void { @assert(1 == 2); };",
                          sema::error::STATIC_ASSERTION_FAILED));
}

TEST_CASE("a comptime-known-false @verify is a compile error") {
    CHECK(helpers::raised("const use := fn(): void { @verify(false, \"nope\"); };",
                          sema::error::STATIC_ASSERTION_FAILED));
}

TEST_CASE("a comptime-known-true @assert / @verify raises nothing") {
    CHECK(helpers::resolver_error_codes(
              "const use := fn(): void { @assert(1 == 1); @verify(2 > 1); };")
              .empty());
}

TEST_CASE("@assert / @verify reject a non-bool condition") {
    CHECK(helpers::raised("const use := fn(p: ^i32): void { @assert(p); };",
                          sema::error::TYPE_MISMATCH));
    CHECK(helpers::raised("const use := fn(n: i32): void { @verify(n); };",
                          sema::error::TYPE_MISMATCH));
}

TEST_CASE("@assert / @verify reject the wrong argument count") {
    CHECK(helpers::raised("const use := fn(): void { @assert(); };", sema::error::ARITY_MISMATCH));
    CHECK(helpers::raised("const use := fn(x: i32): void { @verify(x > 0, \"a\", \"b\"); };",
                          sema::error::ARITY_MISMATCH));
}

} // namespace ghoti::tests
