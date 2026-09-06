#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/statement.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/ast.hh"

namespace ghoti::tests {

namespace {

// The doc attached to the first root declaration's name.
auto first_doc(const ast::AST& ast) -> stdx::option<std::string_view> {
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    return ast.doc_for({ast.location_of(decl.name), ast.end_location_of(decl.name)});
}

} // namespace

TEST_CASE("a leading `///` block attaches to the following declaration") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(/// The answer to everything.
/// Second line.
const answer := 42;
)",
                            arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "The answer to everything.\nSecond line.");
}

TEST_CASE("a trailing same-line `///` attaches to its declaration") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const answer := 42; /// on the same line\n", arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "on the same line");
}

TEST_CASE("plain comments carry no doc") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(// just a comment
const answer := 42;
)",
                            arena)};
    CHECK(!first_doc(ast));
}

TEST_CASE("`//!` at the top of the file documents the module") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(//! This module does things.
//! And more things.
const x := 1;
)",
                            arena)};
    CHECK(ast.module_doc() == "This module does things.\nAnd more things.");
    CHECK(!first_doc(ast));
}

TEST_CASE("a `//!` after real code is not treated as a module doc") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const x := 1;
//! too late
const y := 2;
)",
                            arena)};
    CHECK(ast.module_doc().empty());
}

TEST_CASE("a `///` on a nested declaration does not leak to the next top-level declaration") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const f := fn(): void {
    /// nested
    const local := 1;
    _ = local;
};
const g := 2;
)",
                            arena)};
    const auto&  g{ast.get_as<ast::decl_stmt>(ast.get_roots()[1])};
    CHECK(!ast.doc_for({ast.location_of(g.name), ast.end_location_of(g.name)}));
}

TEST_CASE("doc attachment survives an intervening plain comment") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(/// documented
// noise
const answer := 1;
)",
                            arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "documented");
}

} // namespace ghoti::tests
