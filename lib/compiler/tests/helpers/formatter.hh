#pragma once

#include <string>

#include <stdx/types.hh>

#include "compiler/syntax/doc.hh"

namespace ghoti::tests::helpers {

auto render_docs(syntax::doc_manager& m,
                 syntax::doc_id       root,
                 u16                  max_width     = 100,
                 u16                  indent_spaces = 4) -> std::string;

} // namespace ghoti::tests::helpers
