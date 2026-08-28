#include "helpers/formatter.hh"

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

auto round_trips(std::string_view src) -> void {
    CHECK(ast::dumper::compare_source_asts(src, format_source(src)));     // default width
    CHECK(ast::dumper::compare_source_asts(src, format_source(src, 24))); // forced to break
    CHECK(format_source(format_source(src)) == format_source(src));       // idempotent
}

} // namespace ghoti::tests::helpers
