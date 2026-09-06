#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
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

auto doc_for_name(const ast::AST& ast, ast::identifier_handle name)
    -> stdx::option<std::string_view> {
    return ast.doc_for({ast.location_of(name), ast.end_location_of(name)});
}

// The `value` expression of the first root declaration (e.g. the `struct`/`enum`/`union` body).
template <typename Expr> auto first_decl_value(const ast::AST& ast) -> const Expr& {
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    return ast.get_as<Expr>(*decl.value);
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

TEST_CASE("a leading `///` on a struct field attaches to that field's name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const Point := struct {
    /// The horizontal coordinate.
    x: i32,
    y: i32,
};
)",
                            arena)};
    const auto&  se{first_decl_value<ast::struct_expr>(ast)};
    REQUIRE(se.fields.size() == 2);
    REQUIRE(doc_for_name(ast, se.fields[0].name));
    CHECK(*doc_for_name(ast, se.fields[0].name) == "The horizontal coordinate.");
    CHECK(!doc_for_name(ast, se.fields[1].name));
}

TEST_CASE("a leading `///` on an enum variant attaches to that variant's name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const Color := enum : u32 {
    /// The warm one.
    red = 1u32,
    green = 2u32,
};
)",
                            arena)};
    const auto&  ee{first_decl_value<ast::enum_expr>(ast)};
    REQUIRE(ee.enumerations.size() == 2);
    REQUIRE(doc_for_name(ast, ee.enumerations[0].name));
    CHECK(*doc_for_name(ast, ee.enumerations[0].name) == "The warm one.");
    CHECK(!doc_for_name(ast, ee.enumerations[1].name));
}

TEST_CASE("a leading `///` on a union field attaches to that field's name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const Value := union {
    /// Present when the value is an integer.
    int: i64,
    flag: bool,
};
)",
                            arena)};
    const auto&  ue{first_decl_value<ast::union_expr>(ast)};
    REQUIRE(ue.fields.size() == 2);
    REQUIRE(doc_for_name(ast, ue.fields[0].name));
    CHECK(*doc_for_name(ast, ue.fields[0].name) == "Present when the value is an integer.");
}

TEST_CASE("a leading `///` on a `const` member attaches to that member's name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const Math := struct {
    dummy: u8,
    /// Ratio of a circle's circumference to its diameter.
    const PI := 3;
};
)",
                            arena)};
    const auto&  se{first_decl_value<ast::struct_expr>(ast)};
    REQUIRE(se.members.size() == 1);
    const auto& member_decl{ast.get_as<ast::decl_stmt>(*se.members[0])};
    REQUIRE(doc_for_name(ast, member_decl.name));
    CHECK(*doc_for_name(ast, member_decl.name) ==
          "Ratio of a circle's circumference to its diameter.");
}

TEST_CASE("a trailing same-line `///` attaches to a struct field / enum variant / union field") {
    SECTION("struct field, comma then doc") {
        ghoti::arena arena;
        auto         ast{helpers::parse(R"(const Stat := struct {
    st_dev: i32, /// [XSI] ID of device containing file
    st_mode: u16,
};
)",
                                arena)};
        const auto&  se{first_decl_value<ast::struct_expr>(ast)};
        REQUIRE(se.fields.size() == 2);
        REQUIRE(doc_for_name(ast, se.fields[0].name));
        CHECK(*doc_for_name(ast, se.fields[0].name) == "[XSI] ID of device containing file");
        CHECK(!doc_for_name(ast, se.fields[1].name));
    }

    SECTION("enum variant, comma then doc") {
        ghoti::arena arena;
        auto         ast{helpers::parse(R"(const Errno := enum : i32 {
    EPERM = 1, /// Operation not permitted
    ECHILD = 10,
    _,
};
)",
                                arena)};
        const auto&  ee{first_decl_value<ast::enum_expr>(ast)};
        REQUIRE(ee.enumerations.size() == 2);
        REQUIRE(doc_for_name(ast, ee.enumerations[0].name));
        CHECK(*doc_for_name(ast, ee.enumerations[0].name) == "Operation not permitted");
    }

    SECTION("last field, no trailing comma") {
        ghoti::arena arena;
        auto         ast{helpers::parse(R"(const U := union {
    a: u8,
    b: u16 /// the wide one
};
)",
                                arena)};
        const auto&  ue{first_decl_value<ast::union_expr>(ast)};
        REQUIRE(ue.fields.size() == 2);
        REQUIRE(doc_for_name(ast, ue.fields[1].name));
        CHECK(*doc_for_name(ast, ue.fields[1].name) == "the wide one");
    }
}

TEST_CASE("a `///` leading a struct decl does not leak onto its first field") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(/// A 2D point.
const Point := struct {
    x: i32,
};
)",
                            arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "A 2D point.");
    const auto& se{first_decl_value<ast::struct_expr>(ast)};
    REQUIRE(se.fields.size() == 1);
    CHECK(!doc_for_name(ast, se.fields[0].name));
}

} // namespace ghoti::tests
