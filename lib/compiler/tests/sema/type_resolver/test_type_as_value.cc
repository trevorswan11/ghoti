#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto type_as_value(std::string_view expected, std::string_view found, usize col)
    -> sema::diagnostic {
    return {fmt::format("Expected a value of type '{}', but found the type '{}'", expected, found),
            sema::error::TYPE_USED_AS_VALUE,
            std::pair{0UZ, col}};
}

} // namespace

TEST_CASE("A type alias is rejected where a value of that type is expected") {
    helpers::test_resolver_fail("const S := struct { a: i32 }; const w: S = S;",
                                type_as_value("S", "S", 43UZ));
    helpers::test_resolver_fail("const P := ^i32; const q: ^i32 = P;",
                                type_as_value("^i32", "^i32", 33UZ));
    helpers::test_resolver_fail(
        "const Box := fn(T: type): type { return struct { v: T }; }; const g: bool = Box(i32);",
        type_as_value("bool", "struct", 79UZ));
}

TEST_CASE("A type is rejected in assignment, argument, and return positions") {
    helpers::test_resolver_fail("var x: i32 = 0; const f := fn(): void { x = u8; };",
                                type_as_value("i32", "u8", 44UZ));
    helpers::test_resolver_fail(
        "const f := fn(x: i32): i32 { return x; }; const g := fn(): i32 { return f(i32); };",
        type_as_value("i32", "i32", 74UZ));
    helpers::test_resolver_fail("const f := fn(): i32 { return i32; };",
                                type_as_value("i32", "i32", 30UZ));
}

TEST_CASE("A type is rejected as an aggregate or array element value") {
    helpers::test_resolver_fail("const S := struct { a: i32 }; const s := S{ .a = i32 };",
                                type_as_value("i32", "i32", 49UZ));
    helpers::test_resolver_fail("const a := [2]i32{ 1, i32 };", type_as_value("i32", "i32", 22UZ));
    helpers::test_resolver_fail("const a: [2]i32 = .{ 1, u8 };", type_as_value("i32", "u8", 24UZ));
}

TEST_CASE("Type-accepting slots and ordinary values are unaffected") {
    helpers::resolve_and_check("const f := fn(T: type): usize { return @sizeOf(T); }; const n := f(i32);");
    helpers::resolve_and_check("const x: type = i32;");
    helpers::resolve_and_check("const T := i32; const x: T = 3; const U := T;");
    helpers::resolve_and_check(
        "const Color := enum { red, green }; const c: Color = Color.red; const d: Color = .green;");
    helpers::resolve_and_check(
        "const g := fn(x: auto): usize { return @sizeOf(@TypeOf(x)); }; const n := g(3);");
    helpers::resolve_and_check("var p: i32 = 1; const q: ^i32 = ^p;");
    helpers::resolve_and_check("const S := struct { a: i32 }; const s: S = S{ .a = 1 };");
}

} // namespace ghoti::tests
