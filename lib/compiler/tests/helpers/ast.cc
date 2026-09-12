#include "helpers/ast.hh"

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/ast/ast.hh"
#include "compiler/syntax/parser.hh"

namespace ghoti::tests::helpers {

auto parse(std::string_view source, ghoti::arena& arena) -> ast::AST {
    syntax::parser p{source};
    ast::AST       parsed;
    const auto     errors{p.consume(parsed, arena)};
    REQUIRE(errors.empty());
    return parsed;
}

} // namespace ghoti::tests::helpers
