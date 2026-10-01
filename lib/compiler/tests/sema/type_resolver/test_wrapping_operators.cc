#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("wrapping binary '+%' rejects float operands") {
    helpers::test_resolver_fail(
        "const c := 1.0f32 +% 2.0f32;",
        sema::diagnostic{"operator '+%' expects two integer operands; found 'f32' and 'f32'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 11UZ}});
}

TEST_CASE("wrapping binary '+%' rejects pointer operands") {
    helpers::test_resolver_fail(
        "let mut p: ^i32 = undefined; const x := p +% 1;",
        sema::diagnostic{"operator '+%' expects two integer operands; found '^i32' and "
                         "'comptime_int'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 40UZ}});
}

TEST_CASE("wrapping compound '+%=' rejects boolean operands") {
    helpers::test_resolver_fail(
        "let mut b: bool = true; b +%= true;",
        sema::diagnostic{"operator '+%=' expects two integer operands; found 'bool' and 'bool'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 24UZ}});
}

TEST_CASE("wrapping prefix '-%' rejects float operands") {
    helpers::test_resolver_fail(
        "let mut some_f32: f32 = 1.0f32; const x := -%some_f32;",
        sema::diagnostic{"operator '-%' expects a signed integer operand; found 'f32'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 43UZ}});
}

TEST_CASE("wrapping prefix '-%' rejects unsigned integer operands") {
    helpers::test_resolver_fail(
        "let mut u: u32 = 1u32; const x := -%u;",
        sema::diagnostic{"operator '-%' expects a signed integer operand; found 'u32'",
                         sema::error::OPERATOR_TYPE_MISMATCH,
                         std::pair{0UZ, 34UZ}});
}

TEST_CASE("wrapping operators accept concrete and comptime integers") {
    helpers::resolve_and_check("const c := 1 +% 2;");
    helpers::resolve_and_check("const f := fn(a: u8, b: u8): u8 { return a +% b; };");
    helpers::resolve_and_check("const f := fn(a: i8): i8 { return -%a; };");
    helpers::resolve_and_check("const f := fn(a: u8, b: u8): u8 { return a <<% b; };");
}

} // namespace ghoti::tests
