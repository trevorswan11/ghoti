#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("unary plus is an identity on numeric operands") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            var a: i32 = 2;
            return +a + +3;
        };
    )") == 5);

    CHECK(helpers::raised("const x := +true;", sema::error::OPERATOR_TYPE_MISMATCH));
}

TEST_CASE("a closed range is not a first-class value") {
    CHECK(helpers::raised(R"(
        const f := fn(): void { const r := 1..5; _ = r; };
    )",
                          sema::error::ILLEGAL_OPEN_RANGE));
}

TEST_CASE("an unsuffixed integer literal typed `constexpr_float` folds as a float") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const b: @TypeOf(0.0) = 1;
            if (b != 1.5f64) { return 7; }
            return 0;
        };
    )") == 7);
}

TEST_CASE("a type argument for an `auto` value parameter is rejected, not miscompiled") {
    CHECK(helpers::raised(R"(
        const f := fn(v: auto): u8 { return 1; };
        const g := fn(): u8 { return f(u16); };
    )",
                          sema::error::TYPE_MISMATCH));
}

} // namespace ghoti::tests
