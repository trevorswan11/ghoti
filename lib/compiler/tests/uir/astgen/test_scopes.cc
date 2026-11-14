#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "compiler/sema/error.hh"
#include "helpers/sema.hh"
#include "helpers/uir.hh"

namespace ghoti::tests {

namespace {

using helpers::mock_file;
using sema::error;

auto check_lowering(std::string_view input, std::vector<sema::diagnostic> expected) -> void {
    helpers::check_astgen_against(input, {}, expected);
}

auto check_lowers(std::string_view input) -> void { check_lowering(input, {}); }

} // namespace

TEST_CASE("A local may not take a name a later module declaration takes") {
    check_lowering("const f = fn(): i32 { let later = 2; return later; };\nconst later = 5;",
                   {{"Attempt to shadow identifier 'later'; previous declaration here: 2:7",
                     error::SHADOWING_DECLARATION,
                     std::pair{0UZ, 26UZ}}});
}

TEST_CASE("An inner local may reuse a name its enclosing block declares further on") {
    check_lowers("const f = fn(): i32 { { let x = 1; _ = x; } let x = 2; return x; };");
}

TEST_CASE("A loop capture may reuse a name its function declares further on") {
    check_lowers(
        "const f = fn(n: usize): usize { for (0..n) |i| { _ = i; } let mut i = n; return i; };");
}

TEST_CASE("An inner local may not take a name its enclosing block already declares") {
    check_lowering("const f = fn(): i32 { let x = 2; { let x = 1; _ = x; } return x; };",
                   {{"Attempt to shadow identifier 'x'; previous declaration here: 1:27",
                     error::SHADOWING_DECLARATION,
                     std::pair{0UZ, 39UZ}}});
}

TEST_CASE("A member declared after a local of the same name is still shadowed") {
    check_lowering(
        "const S = struct { pub const f = fn(): i32 { let k = 1; return k; }; pub const k = 3; };",
        {{"Attempt to shadow identifier 'k'; previous declaration here: 1:80",
          error::SHADOWING_DECLARATION,
          std::pair{0UZ, 49UZ}}});
}

TEST_CASE("A shadowing local still binds, so its uses report nothing more") {
    check_lowering("const x = 1;\nconst f = fn(): i32 { let x = 2; return x + x; };",
                   {{"Attempt to shadow identifier 'x'; previous declaration here: 1:7",
                     error::SHADOWING_DECLARATION,
                     std::pair{1UZ, 26UZ}}});
}

TEST_CASE("The prelude's module name can't be shadowed") {
    check_lowering("const f = fn(): void { let builtin = 1; _ = builtin; };",
                   {{"Attempt to shadow identifier 'builtin', which the prelude declares",
                     error::SHADOWING_DECLARATION,
                     std::pair{0UZ, 27UZ}}});
}

TEST_CASE("Members may reuse an outer name but not their own type's") {
    check_lowers("const k = 1;\nconst S = struct { pub const k = 2; };");
    check_lowering("const S = struct { pub const S = 2; };",
                   {{"Attempt to shadow identifier 'S'; previous declaration here: 1:7",
                     error::SHADOWING_DECLARATION,
                     std::pair{0UZ, 29UZ}}});
}

TEST_CASE("A bodyless signature's parameter names bind nothing a body reads") {
    const std::vector<mock_file> imports{{.path = "spec.gh", .source = "pub const v = 1;"}};
    helpers::check_astgen_against("import \"spec.gh\" as spec;\nconst I = interface { const f = "
                                  "fn(&self, spec: u8): void; };",
                                  imports,
                                  std::vector<sema::diagnostic>{});
}

TEST_CASE("A local used before its declaration is an error") {
    check_lowering("const f = fn(): i32 { let a = b; let b = 1; return a; };",
                   {{"'b' is used before its declaration",
                     error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
                     std::pair{0UZ, 30UZ}}});
}

TEST_CASE("A local const function calling a later one is used before its declaration") {
    // The old pipeline crashes on this
    check_lowering("pub const main = fn(): i32 {\n"
                   "    const f = fn(): i32 { return g(); };\n"
                   "    const g = fn(): i32 { return 3; };\n"
                   "    return f();\n"
                   "};",
                   {{"'g' is used before its declaration",
                     error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
                     std::pair{1UZ, 33UZ}}});
}

TEST_CASE("Module scope is order independent") {
    check_lowers("const a = b + 1;\nconst b = 2;\nconst f = fn(): i32 { return g(); };\n"
                 "const g = fn(): i32 { return a; };");
}

TEST_CASE("An undeclared name is reported where it is used") {
    check_lowering("const f = fn(): i32 { return missing; };",
                   {{"Use of undeclared identifier 'missing'",
                     error::UNDECLARED_IDENTIFIER,
                     std::pair{0UZ, 29UZ}}});
}

TEST_CASE("A field named bare inside a method is reached through an instance") {
    check_lowering("const P = struct { x: i32, pub const get = fn(self): i32 { return x; }; };",
                   {{"'x' is a field; access it through an instance, e.g. `self.x`",
                     error::UNDECLARED_IDENTIFIER,
                     std::pair{0UZ, 66UZ}}});
}

TEST_CASE("A missing import is a load error at its path") {
    check_lowering("import \"nowhere.gh\" as nowhere;",
                   {{"Could not find path 'nowhere.gh' in virtual file system",
                     error::MODULE_LOAD_ERROR,
                     std::pair{0UZ, 7UZ}}});
}

TEST_CASE("An import inside a test is visible only there") {
    const std::vector<mock_file> imports{{.path = "lib.gh", .source = "pub const v = 1;"}};
    helpers::check_astgen_against(
        "test { import \"lib.gh\" as lib; _ = lib.v; }\nconst f = fn(): i32 { return lib.v; };",
        imports,
        std::vector<sema::diagnostic>{{"Use of undeclared identifier 'lib'",
                                       error::UNDECLARED_IDENTIFIER,
                                       std::pair{1UZ, 29UZ}}});
}

TEST_CASE("A labeled jump needs a label that encloses it") {
    check_lowering("const f = fn(): void { loop { break :nowhere; } };",
                   {{"Labeled break statements must be used with a known label",
                     error::ILLEGAL_CONTROL_FLOW,
                     std::pair{0UZ, 37UZ}}});
    check_lowering("const f = fn(): void { blk: { continue :blk; } };",
                   {{"Labeled continue statements must be used with a known label",
                     error::ILLEGAL_CONTROL_FLOW,
                     std::pair{0UZ, 40UZ}}});
}

TEST_CASE("A jump may not leave a defer body") {
    check_lowering(
        "const f = fn(): void { loop { defer { break; } } };",
        {{"A break cannot leave a defer body", error::ILLEGAL_CONTROL_FLOW, std::pair{0UZ, 38UZ}}});
    check_lowering("const f = fn(): i32 { defer { return 1; } return 0; };",
                   {{"A return cannot leave a defer body",
                     error::ILLEGAL_CONTROL_FLOW,
                     std::pair{0UZ, 30UZ}}});
    check_lowers("const f = fn(): void { defer { loop { break; } } };");
}

} // namespace ghoti::tests
