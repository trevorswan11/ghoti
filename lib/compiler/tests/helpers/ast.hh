#pragma once

#include <concepts>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "helpers/common.hh"

namespace ghoti::tests::helpers {

// Tests a syntactically failing input against the expected generated errors
template <std::same_as<syntax::diagnostic>... Ds>
auto test_parser_fail(std::string_view failing, Ds&&... expected_diagnostics) -> void {
    syntax::parser p{failing};
    ghoti::arena   arena;
    ast::AST       ast;
    auto           errors{p.consume(ast, arena)};
    REQUIRE(ast.empty());
    helpers::check_errors_against<syntax::diagnostic>(errors,
                                                      std::forward<Ds>(expected_diagnostics)...);
}

// Parses `source` into `arena` (which must outlive the returned AST) and asserts it is clean.
auto parse(std::string_view source, ghoti::arena& arena) -> ast::AST;

} // namespace ghoti::tests::helpers
