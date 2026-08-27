#include "helpers/formatter.hh"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>

#include "compiler/ast/dumper.hh"
#include "compiler/ast/formatter.hh"
#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"
#include "compiler/syntax/parser.hh"

namespace ghoti::tests::helpers {

auto render_docs(syntax::doc_manager& m, syntax::doc_id root, u16 max_width, u16 indent_spaces)
    -> std::string {
    std::ostringstream os;
    syntax::layout_engine{m, max_width, indent_spaces}.render(root, os);
    return os.str();
}

auto format_source(std::string_view src, u16 max_width, u16 indent_spaces) -> std::string {
    syntax::parser p{src};
    ast::AST       ast;
    const auto     errors{p.consume(ast)};
    CHECK(errors.empty());

    std::ostringstream os;
    ast::formatter{ast, os, max_width, indent_spaces}.format();
    return os.str();
}

auto check_sources_equiv(std::string_view s1, std::string_view s2) -> void {
    syntax::parser p1{s1}, p2{s2};
    ast::AST       s1_ast, s2_ast;
    const auto     diag1{p1.consume(s1_ast)};
    const auto     diag2{p2.consume(s2_ast)};
    CHECK(std::ranges::equal(diag1, diag2));
    if (!diag1.empty() || !diag2.empty()) { return; }

    std::ostringstream s1_oss, s2_oss;
    ast::dumper        dumper1{s1_ast, s1_oss}, dumper2{s2_ast, s2_oss};
    dumper1.dump();
    dumper2.dump();
    CHECK(s1_oss.view() == s2_oss.view());
}

} // namespace ghoti::tests::helpers
