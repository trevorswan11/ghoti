#pragma once

#include <stdx/variant.hh>

// IWYU pragma: begin_keep
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
// IWYU pragma: end_keep

namespace ghoti::ast {

#define X(Type) Type,
using node_variant = stdx::variant<FOREACH_AST_NODE(X) discarded>;
#undef X

#define AST_NODE_VISITOR_NOOP(Class, NodeType) \
    auto Class::visit(ghoti::ast::node_id, const ghoti::ast::NodeType&) -> void {}

using type_variant = stdx::variant<identifier_expr,
                                   module_access_expr,
                                   dot_expr,
                                   call_expr,
                                   explicit_function_type,
                                   explicit_type_id,
                                   struct_expr,
                                   enum_expr,
                                   union_expr,
                                   interface_expr,
                                   explicit_array_type>;

#define AST_TYPE_VISITOR_NOOP(Class, NodeType) \
    auto Class::visit(ghoti::ast::explicit_type_id, const ghoti::ast::NodeType&) -> void {}

// Creates the template instantiation for a ID-templated, Node/ExplicitType ID visitor
#define VISITOR_TEMPLATE_INIT(ClassName, fn_name, NodeType)                                      \
    template auto ClassName::fn_name<ghoti::ast::node_id>(ghoti::ast::node_id, NodeType)->void;  \
    template auto ClassName::fn_name<ghoti::ast::explicit_type_id>(ghoti::ast::explicit_type_id, \
                                                                   NodeType)                     \
        ->void;

} // namespace ghoti::ast
