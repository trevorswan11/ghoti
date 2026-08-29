#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/syntax/parser.hh"

namespace ghoti::tests {

namespace {

auto parse(std::string_view source) -> ast::AST {
    syntax::parser p{source};
    ast::AST       parsed;
    const auto     errors{p.consume(parsed)};
    REQUIRE(errors.empty());
    return parsed;
}

} // namespace

TEST_CASE("every node has an end_location_of that is not before its start location_of") {
    auto ast{parse("const x := foo(1, 2) + bar.baz;\n"
                   "const y := struct { a: i32 };\n")};

    for (const auto root : ast) {
        const auto& start{ast.location_of(root)};
        const auto& end{ast.end_location_of(root)};
        CHECK((end.line > start.line || (end.line == start.line && end.column >= start.column)));
    }
}

TEST_CASE("a call_expr's span ends just past its closing paren") {
    auto        ast{parse("const x := foo(1, 2);\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& call{ast.get_as<ast::call_expr>(*decl.value)};
    const auto& end{ast.end_location_of(*decl.value)};

    CHECK(end.line == 0);
    CHECK(end.column == 20);
    CHECK(call.arguments.size() == 2);
}

TEST_CASE("a binary_expr's span starts at its lhs, not its operator") {
    auto        ast{parse("const x := 1 + 2;\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& bin{ast.get_as<ast::binary_expr>(*decl.value)};
    const auto& start{ast.location_of(*decl.value)};
    CHECK(bin.rhs.is<ast::i32_expr>());

    CHECK(start.line == 0);
    CHECK(start.column == 11);
}

TEST_CASE("an assignment_expr's span starts at its lhs, not its operator") {
    auto        ast{parse("const f := fn(): void {\n"
                          "    var x := 1;\n"
                          "    x = 2;\n"
                          "};\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};
    REQUIRE(decl.value);
    const auto& fn{ast.get_as<ast::function_expr>(*decl.value)};
    const auto& body{ast.get_as<ast::block_stmt>(*fn.body)};

    REQUIRE(body.statements.size() == 2);
    const auto& assign_stmt{ast.get_as<ast::expr_stmt>(*body.statements[1])};
    const auto& assign{ast.get_as<ast::assignment_expr>(*assign_stmt.expression)};
    const auto& start{ast.location_of(*assign_stmt.expression)};
    CHECK(assign.lhs.is<ast::identifier_expr>());

    CHECK(start.line == 2);
    CHECK(start.column == 4);
}

TEST_CASE("a postfix unwrap_expr's span starts at its operand and ends past its operator") {
    auto        ast{parse("const x := abc?;\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& unwrap{ast.get_as<ast::unwrap_expr>(*decl.value)};
    CHECK((**decl.value).get_token_type() == syntax::token_type_t::QUESTION);
    CHECK(unwrap.operand.is<ast::identifier_expr>());

    const auto& start{ast.location_of(*decl.value)};
    const auto& end{ast.end_location_of(*decl.value)};
    CHECK(start.line == 0);
    CHECK(start.column == 11); // the `a` of `abc`, not the `?`
    CHECK(end.line == 0);
    CHECK(end.column == 15); // just past the `?`
}

TEST_CASE("a block_stmt's span ends just past its closing brace") {
    auto        ast{parse("const f := fn(): void {\n"
                          "    return;\n"
                          "};\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& fn{ast.get_as<ast::function_expr>(*decl.value)};
    const auto& end{ast.end_location_of(fn.body)};

    CHECK(end.line == 2);
    CHECK(end.column == 1);
}

} // namespace ghoti::tests
