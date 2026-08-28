#include "driver/cmd/lsp/formatting.hh"

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/dumper.hh"
#include "compiler/ast/formatter.hh"
#include "compiler/syntax/parser.hh"

namespace ghoti::lsp {

namespace {

[[nodiscard]] auto full_document_range(std::string_view text) -> nlohmann::json {
    usize line{0};
    usize last_newline{0};
    bool  has_newline{false};

    for (usize i{0}; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            last_newline = i + 1;
            has_newline  = true;
        }
    }

    const usize character{has_newline ? text.size() - last_newline : text.size()};

    return {
        {"start", {{"line", 0}, {"character", 0}}},
        {"end", {{"line", line}, {"character", character}}},
    };
}

[[nodiscard]] auto format_document(std::string_view source_code, formatting_options opts)
    -> stdx::option<std::string> {
    syntax::parser parser{source_code};
    ast::AST       ast;
    const auto     diagnostics{parser.consume(ast)};
    if (!diagnostics.empty()) { return stdx::none; }

    std::ostringstream formatted_os;
    ast::formatter     ast_fmt{ast, formatted_os, opts.max_width, opts.indent_spaces};
    ast_fmt.format();
    std::string formatted_code{formatted_os.str()};

    // Verify ASTs for each are valid before returning result
    if (!ast::dumper::compare_source_asts(source_code, formatted_code)) { return stdx::none; }
    return formatted_code;
}

} // namespace

auto format(std::string_view source_code, formatting_options opts) -> nlohmann::json {
    auto formatted{format_document(source_code, opts)};
    if (!formatted) { return nullptr; }

    if (*formatted == source_code) { return nlohmann::json::array(); }

    auto edits = nlohmann::json::array();
    edits.push_back({
        {"range", full_document_range(source_code)},
        {"newText", std::move(*formatted)},
    });
    return edits;
}

} // namespace ghoti::lsp
