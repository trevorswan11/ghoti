#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("`constexpr` on a `type` parameter is redundant") {
    helpers::test_resolver_fail(
        "const f := fn(constexpr t: type): i32 { _ = t; return 0; };",
        sema::diagnostic{"'constexpr' is redundant on a parameter of type 'type'; type values "
                         "are always compile-time known",
                         sema::error::REDUNDANT_CONSTEXPR,
                         std::pair{0UZ, 24UZ}});
}

TEST_CASE("`constexpr` on a parameter typed by an earlier generic type param isn't redundant") {
    helpers::resolve_and_check(
        "const Point := fn(T: type, constexpr default_z: T): type { return T; };");
}

TEST_CASE("a `var` binding cannot hold a `type` value") {
    const auto mutable_type_diag = [](usize col) {
        return sema::diagnostic{"a 'type' value cannot be stored in a mutable ('var') binding; "
                                "use 'const' or 'constexpr' instead",
                                sema::error::MUTABLE_TYPE_BINDING,
                                std::pair{0UZ, col}};
    };

    helpers::test_resolver_fail("var a: type = i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := ^mut i32;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("var a := []u8;", mutable_type_diag(0UZ));
    helpers::test_resolver_fail("const S := struct { x: i32 }; var a := S;",
                                mutable_type_diag(30UZ));
    helpers::test_resolver_fail(
        "const Box := fn(T: type): type { return struct { v: T }; }; var a := Box(i32);",
        mutable_type_diag(60UZ));

    helpers::resolve_and_check("const a: type = i32;");
    helpers::resolve_and_check("const a := i32;");
    helpers::resolve_and_check("constexpr a: type = i32;");
    helpers::resolve_and_check("constexpr a := i32;");
    helpers::resolve_and_check("const S := struct { x: i32 };");
    helpers::resolve_and_check("const f := fn(t: type): i32 { _ = t; return 0; };");

    helpers::resolve_and_check("const P := struct { x: i32 }; var p: P = undefined;");
    helpers::resolve_and_check("var arr: [2uz]i32 = [_]i32{1, 2};");
}

} // namespace ghoti::tests
