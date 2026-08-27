#include "helpers/formatter.hh"

#include <sstream>
#include <string>

#include <stdx/types.hh>

#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"

namespace ghoti::tests::helpers {

auto render_docs(syntax::doc_manager& m, syntax::doc_id root, u16 max_width, u16 indent_spaces)
    -> std::string {
    std::ostringstream os;
    syntax::layout_engine{m, max_width, indent_spaces}.render(root, os);
    return os.str();
}

} // namespace ghoti::tests::helpers
