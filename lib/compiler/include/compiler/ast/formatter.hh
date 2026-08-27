#pragma once

#include <ostream>

#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/doc.hh"

namespace ghoti::ast {

class formatter {
  public:
    explicit formatter(const AST&    ast,
                       std::ostream& out,
                       u16           max_width     = 100,
                       u16           indent_spaces = 4) noexcept
        : out_{out}, ast_{ast}, max_width_{max_width}, indent_spaces_{indent_spaces} {}

    auto format() -> void;

  private:
    template <IndexableID ID> auto format(ID id) -> syntax::doc_id {
        return ast_[id].visit([&](const auto& data) { return this->visit(id, data); });
    }

    auto visit(node_id, const array_expr&) -> syntax::doc_id;
    auto visit(node_id, const call_expr&) -> syntax::doc_id;
    auto visit(node_id, const do_while_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const enum_expr&) -> syntax::doc_id;
    auto visit(node_id, const for_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const function_expr&) -> syntax::doc_id;
    auto visit(node_id, const identifier_expr&) -> syntax::doc_id;
    auto visit(node_id, const if_expr&) -> syntax::doc_id;
    auto visit(node_id, const index_expr&) -> syntax::doc_id;
    auto visit(node_id, const infinite_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const assignment_expr&) -> syntax::doc_id;
    auto visit(node_id, const binary_expr&) -> syntax::doc_id;
    auto visit(node_id, const dot_expr&) -> syntax::doc_id;
    auto visit(node_id, const range_expr&) -> syntax::doc_id;
    auto visit(node_id, const initializer_expr&) -> syntax::doc_id;
    auto visit(node_id, const label_expr&) -> syntax::doc_id;
    auto visit(node_id, const match_expr&) -> syntax::doc_id;
    auto visit(node_id, const reference_expr&) -> syntax::doc_id;
    auto visit(node_id, const address_of_expr&) -> syntax::doc_id;
    auto visit(node_id, const dereference_expr&) -> syntax::doc_id;
    auto visit(node_id, const unary_expr&) -> syntax::doc_id;
    auto visit(node_id, const implicit_access_expr&) -> syntax::doc_id;
    auto visit(node_id, const string_expr&) -> syntax::doc_id;
    auto visit(node_id, const i32_expr&) -> syntax::doc_id;
    auto visit(node_id, const i64_expr&) -> syntax::doc_id;
    auto visit(node_id, const isize_expr&) -> syntax::doc_id;
    auto visit(node_id, const u32_expr&) -> syntax::doc_id;
    auto visit(node_id, const u64_expr&) -> syntax::doc_id;
    auto visit(node_id, const usize_expr&) -> syntax::doc_id;
    auto visit(node_id, const u8_expr&) -> syntax::doc_id;
    auto visit(node_id, const f32_expr&) -> syntax::doc_id;
    auto visit(node_id, const f64_expr&) -> syntax::doc_id;
    auto visit(node_id, const bool_expr&) -> syntax::doc_id;
    auto visit(node_id, const void_expr&) -> syntax::doc_id;
    auto visit(node_id, const undefined_expr&) -> syntax::doc_id;
    auto visit(node_id, const nullptr_expr&) -> syntax::doc_id;
    auto visit(node_id, const unreachable_expr&) -> syntax::doc_id;
    auto visit(node_id, const module_access_expr&) -> syntax::doc_id;
    auto visit(node_id, const struct_expr&) -> syntax::doc_id;
    auto visit(node_id, const union_expr&) -> syntax::doc_id;
    auto visit(node_id, const while_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const block_stmt&) -> syntax::doc_id;
    auto visit(node_id, const break_stmt&) -> syntax::doc_id;
    auto visit(node_id, const continue_stmt&) -> syntax::doc_id;
    auto visit(node_id, const decl_stmt&) -> syntax::doc_id;
    auto visit(node_id, const defer_stmt&) -> syntax::doc_id;
    auto visit(node_id, const discard_stmt&) -> syntax::doc_id;
    auto visit(node_id, const expr_stmt&) -> syntax::doc_id;
    auto visit(node_id, const import_stmt&) -> syntax::doc_id;
    auto visit(node_id, const return_stmt&) -> syntax::doc_id;
    auto visit(node_id, const test_stmt&) -> syntax::doc_id;
    auto visit(node_id, const using_stmt&) -> syntax::doc_id;
    auto visit(node_id, stdx::monostate) -> syntax::doc_id;

    auto visit(explicit_type_id, const identifier_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const module_access_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const dot_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const call_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_function_type&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_type_id&) -> syntax::doc_id;
    auto visit(explicit_type_id, const struct_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const enum_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const union_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_array_type&) -> syntax::doc_id;

  private:
    std::ostream&       out_;
    const AST&          ast_;
    syntax::doc_manager doc_manager_;
    u16                 max_width_;
    u16                 indent_spaces_;
};

} // namespace ghoti::ast
