#pragma once

#include <string>
#include <string_view>

#include <stdx/types.hh>

#include "compiler/syntax/doc.hh"

namespace ghoti::tests::helpers {

auto render_docs(syntax::doc_manager& m,
                 syntax::doc_id       root,
                 u16                  max_width     = 100,
                 u16                  indent_spaces = 4) -> std::string;

// If either input has errors, dumped ast output is never compared
auto check_sources_equiv(std::string_view s1, std::string_view s2) -> void;

} // namespace ghoti::tests::helpers
