#include "compiler/ast/ast.hh"

#include <stdx/profiler.hh>

#include "compiler/arena.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"

namespace ghoti::ast {

auto AST::borrow(ghoti::arena& arena) const -> AST {
    PROFILE_FUNCTION();
    AST out;
    out.nodes_          = nodes_;
    out.explicit_types_ = explicit_types_;
    out.doc_comments_   = doc_comments_;
    out.module_doc_     = module_doc_;
    out.set_arena(arena);

    // Sema rewrites these in place (cfg splicing, call targets), so they get their own copies now
    // rather than moving mid-analysis
    out.shared_nodes_.assign(nodes_.pool.size(), true);
    for (const auto id : nodes_.ids) {
        if (id.is<block_stmt>() || id.is<call_expr>() || id.is<struct_expr>() ||
            id.is<union_expr>() || id.is<enum_expr>()) {
            out.own_node(id.get_index());
        }
    }
    return out;
}

} // namespace ghoti::ast
