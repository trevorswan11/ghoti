#include "compiler/ast/formatter.hh"

#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"

namespace ghoti::ast {

auto formatter::format() -> void {
    for (const auto id : ast_) {
        doc_manager_.add_root(format(id));
        doc_manager_.add_root(doc_manager_.add<syntax::docs::hard_line>());
    }

    syntax::layout_engine solver{doc_manager_, max_width_, indent_spaces_};
    solver.render(out_);
}

auto formatter::visit(node_id id, const array_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const call_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const do_while_loop_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const enum_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const for_loop_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const function_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const identifier_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const if_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const index_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const infinite_loop_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const assignment_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const binary_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const dot_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const range_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const initializer_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const label_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const match_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const reference_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const address_of_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const dereference_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const unary_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const implicit_access_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const string_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const i32_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const i64_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const isize_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const u32_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const u64_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const usize_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const u8_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const f32_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const f64_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const bool_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const void_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const undefined_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const nullptr_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const unreachable_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const module_access_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(node_id id, const struct_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const union_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const while_loop_expr& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const block_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const break_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const continue_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const decl_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const defer_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const discard_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const expr_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const import_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const return_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const test_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, const using_stmt& node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(node_id id, stdx::monostate node) -> syntax::doc_id { TODO(id, node); }

auto formatter::visit(explicit_type_id id, const identifier_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const module_access_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const dot_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const call_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const explicit_function_type& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const explicit_type_id& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const struct_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const enum_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const union_expr& node) -> syntax::doc_id {
    TODO(id, node);
}

auto formatter::visit(explicit_type_id id, const explicit_array_type& node) -> syntax::doc_id {
    TODO(id, node);
}

} // namespace ghoti::ast
