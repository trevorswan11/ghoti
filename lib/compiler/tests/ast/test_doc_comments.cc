#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/syntax/error.hh"
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

// The `MISPLACED_DOC_COMMENT` errors parsing `source` reports.
auto misplaced_docs(std::string_view source) -> usize {
    syntax::parser p{source};
    ghoti::arena   arena;
    ast::AST       ast;
    usize          count{0};
    for (const auto& diag : p.consume(ast, arena)) {
        if (diag.get_error() == syntax::error::MISPLACED_DOC_COMMENT) { ++count; }
    }
    return count;
}

} // namespace

TEST_CASE("a leading `///` block attaches to the following declaration") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(/// The answer to everything.
/// Second line.
const answer = 42;
)",
                                    arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "The answer to everything.\nSecond line.");
}

TEST_CASE("a trailing same-line `///` attaches to its declaration") {
    ghoti::arena arena;
    auto         ast{helpers::parse("const answer = 42; /// on the same line\n", arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "on the same line");
}

TEST_CASE("plain comments carry no doc") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(// just a comment
const answer = 42;
)",
                                    arena)};
    CHECK(!first_doc(ast));
}

TEST_CASE("`//!` at the top of the file documents the module") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(//! This module does things.
//! And more things.
const x = 1;
)",
                                    arena)};
    CHECK(ast.module_doc() == "This module does things.\nAnd more things.");
    CHECK(!first_doc(ast));
}

TEST_CASE("doc attachment survives an intervening plain comment") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(/// documented
// noise
const answer = 1;
)",
                                    arena)};
    REQUIRE(first_doc(ast));
    CHECK(*first_doc(ast) == "documented");
}

TEST_CASE("a leading `///` on a struct field attaches to that field's name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const Point = struct {
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
    auto         ast{helpers::parse(R"(const Color = enum : u32 {
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
    auto         ast{helpers::parse(R"(const Value = union {
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
    auto         ast{helpers::parse(R"(const Math = struct {
    dummy: u8,
    /// Ratio of a circle's circumference to its diameter.
    const PI = 3;
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
        auto         ast{helpers::parse(R"(const Stat = struct {
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
        auto         ast{helpers::parse(R"(const Errno = enum : i32 {
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
        auto         ast{helpers::parse(R"(const U = union {
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
const Point = struct {
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

TEST_CASE("a `///` that documents nothing is an error") {
    SECTION("Statements inside a function body") {
        CHECK(misplaced_docs(R"(const f = fn(): void {
    /// nested
    let local = 1;
    _ = local;
};
)") == 1);
    }

    SECTION("A test block, an `impl` block, and a `@cfg` block are not declarations") {
        CHECK(misplaced_docs(R"(/// a test
test "t" {}
)") == 1);
        CHECK(misplaced_docs(R"(const S = struct {};
const I = interface {};
/// an impl
impl I for S {}
)") == 1);
        CHECK(misplaced_docs(R"(/// a cfg block
@cfg (os == .linux) {
    const a = 1;
}
)") == 1);
    }

    SECTION("Trailing the file with nothing after it") {
        CHECK(misplaced_docs(R"(const a = 1;
/// dangling
)") == 1);
    }

    SECTION("A `//!` after code") {
        CHECK(misplaced_docs(R"(const x = 1;
//! too late
const y = 2;
)") == 1);
    }
}

TEST_CASE("a `///` in a logical location is accepted") {
    CHECK(misplaced_docs(R"(//! module docs
/// a decl
const a = 1; /// trailing
/// an aggregate
const S = struct {
    /// a field
    x: i32,
    /// a method
    pub const get = fn(
        /// the receiver
        &self,
    ): i32 {
        return self.x;
    };
};
/// an interface
const I = interface {
    /// an associated type
    Item: type;
    /// an associated const
    const N: usize;
    /// a method
    const next = fn(
        &mut self,
        /// how many to skip
        skip: usize,
    ): Item;
};
@cfg (os == .linux) {
    /// a decl in a cfg block
    const b = 2;
}
)") == 0);
}

TEST_CASE("a leading or trailing `///` on a parameter attaches to its name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const f = fn(
    /// the first
    a: i32,
    b: i32, /// the second
): i32 {
    return a + b;
};
)",
                                    arena)};
    const auto&  decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    const auto&  fn{ast.get_as<ast::function_expr>(*decl.value)};
    REQUIRE(fn.parameters.size() == 2);
    const auto name_of{[&](usize i) { return ast::identifier_handle{fn.parameters[i].name}; }};
    REQUIRE(doc_for_name(ast, name_of(0)));
    CHECK(*doc_for_name(ast, name_of(0)) == "the first");
    REQUIRE(doc_for_name(ast, name_of(1)));
    CHECK(*doc_for_name(ast, name_of(1)) == "the second");
}

TEST_CASE("a `///` on an interface member attaches to its name") {
    ghoti::arena arena;
    auto         ast{helpers::parse(R"(const I = interface {
    /// the item
    Item: type;
    /// advances
    const next = fn(&mut self): Item;
};
)",
                                    arena)};
    const auto&  ie{first_decl_value<ast::interface_expr>(ast)};
    REQUIRE(doc_for_name(ast, ie.assoc_types[0].name));
    CHECK(*doc_for_name(ast, ie.assoc_types[0].name) == "the item");
    REQUIRE(doc_for_name(ast, ie.methods[0].name));
    CHECK(*doc_for_name(ast, ie.methods[0].name) == "advances");
}

} // namespace ghoti::tests
