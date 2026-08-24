#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
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
    auto ast{parse("const x := foo(1, 2);\n")};
    const auto& decl{ast.get_as<ast::decl_stmt>(*ast.begin())};

    REQUIRE(decl.value);
    const auto& call{ast.get_as<ast::call_expr>(*decl.value)};
    const auto& end{ast.end_location_of(*decl.value)};

    CHECK(end.line == 0);
    CHECK(end.column == 20);
    CHECK(call.arguments.size() == 2);
}

TEST_CASE("a block_stmt's span ends just past its closing brace") {
    auto ast{parse("const f := fn(): void {\n"
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
