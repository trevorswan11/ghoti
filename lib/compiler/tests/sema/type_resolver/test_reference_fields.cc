#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Reference-typed fields are allowed in plain structs and unions") {
    helpers::resolve_and_check("const S := struct { r: &i32, m: &mut i32 };");
    helpers::resolve_and_check("const U := union { r: &i32, n: i32 };");
}

TEST_CASE("A reference-typed field is rejected in an extern struct") {
    helpers::test_resolver_fail(
        "const S := extern struct { r: &i32 };",
        sema::diagnostic{"extern struct field 'r' cannot have a reference type; an 'extern' "
                         "aggregate has no ABI representation for references, use a raw pointer "
                         "('^T') instead",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 30UZ}});
}

TEST_CASE("A reference-typed field is rejected in an extern union") {
    helpers::test_resolver_fail(
        "const U := extern union { r: &mut i32 };",
        sema::diagnostic{"extern union field 'r' cannot have a reference type; an 'extern' "
                         "aggregate has no ABI representation for references, use a raw pointer "
                         "('^T') instead",
                         sema::error::ILLEGAL_REFERENCE_FIELD,
                         std::pair{0UZ, 29UZ}});
}

} // namespace ghoti::tests
