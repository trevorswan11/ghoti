#pragma once

#include <string_view>
#include <utility>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "support/counter.hh"

namespace ghoti::sema {

// An AST walker that performs 0 type checking
class symbol_collector {
  public:
    static auto collect_symbols(mod::module& module, context& ctx) -> mod::module_state;

    template <ast::IndexableID ID> auto collect(ID id) -> void {
        collecting_.ast[id].visit([&](const auto& data) -> void { visit(id, data); });
    }

  private:
    using scope = symbol_table_stack::scope;

  private:
    auto                                visit(ast::node_id, const ast::array_expr&) -> void;
    auto                                visit(ast::node_id, const ast::asm_expr&) -> void;
    auto                                visit(ast::node_id, const ast::call_expr&) -> void;
    auto                                visit(ast::node_id, const ast::do_while_loop_expr&) -> void;
    template <ast::IndexableID ID> auto visit(ID, const ast::enum_expr&) -> void;
    auto                                visit(ast::node_id, const ast::for_loop_expr&) -> void;
    auto                                visit(ast::node_id, const ast::function_expr&) -> void;
    auto                                visit(ast::node_id, const ast::identifier_expr&) -> void;
    auto                                visit(ast::node_id, const ast::if_expr&) -> void;
    auto                                visit(ast::node_id, const ast::index_expr&) -> void;
    auto                                visit(ast::node_id, const ast::infinite_loop_expr&) -> void;
    auto                                visit(ast::node_id, const ast::cfg_value_expr&) -> void;
    auto                                visit(ast::node_id, const ast::assignment_expr&) -> void;
    auto                                visit(ast::node_id, const ast::binary_expr&) -> void;
    auto                                visit(ast::node_id, const ast::dot_expr&) -> void;
    auto                                visit(ast::node_id, const ast::range_expr&) -> void;
    auto                                visit(ast::node_id, const ast::initializer_expr&) -> void;
    auto                                visit(ast::node_id, const ast::label_expr&) -> void;
    auto                                visit(ast::node_id, const ast::match_expr&) -> void;
    auto                                visit(ast::node_id, const ast::reference_expr&) -> void;
    auto                                visit(ast::node_id, const ast::address_of_expr&) -> void;
    auto                                visit(ast::node_id, const ast::dereference_expr&) -> void;
    auto                                visit(ast::node_id, const ast::unary_expr&) -> void;
    auto                                visit(ast::node_id, const ast::unwrap_expr&) -> void;
    auto visit(ast::node_id, const ast::implicit_access_expr&) -> void;
    auto visit(ast::node_id, const ast::string_expr&) -> void;
    auto visit(ast::node_id, const ast::i32_expr&) -> void;
    auto visit(ast::node_id, const ast::i64_expr&) -> void;
    auto visit(ast::node_id, const ast::isize_expr&) -> void;
    auto visit(ast::node_id, const ast::u32_expr&) -> void;
    auto visit(ast::node_id, const ast::u64_expr&) -> void;
    auto visit(ast::node_id, const ast::usize_expr&) -> void;
    auto visit(ast::node_id, const ast::u8_expr&) -> void;
    auto visit(ast::node_id, const ast::f32_expr&) -> void;
    auto visit(ast::node_id, const ast::f64_expr&) -> void;
    auto visit(ast::node_id, const ast::bool_expr&) -> void;
    auto visit(ast::node_id, const ast::void_expr&) -> void;
    auto visit(ast::node_id, const ast::undefined_expr&) -> void;
    auto visit(ast::node_id, const ast::nullptr_expr&) -> void;
    auto visit(ast::node_id, const ast::unreachable_expr&) -> void;
    auto visit(ast::node_id, const ast::module_access_expr&) -> void;
    template <ast::IndexableID ID> auto visit(ID, const ast::struct_expr&) -> void;
    template <ast::IndexableID ID> auto visit(ID, const ast::union_expr&) -> void;
    auto                                visit(ast::node_id, const ast::while_loop_expr&) -> void;
    auto                                visit(ast::node_id, ast::discarded) noexcept -> void {}

    auto visit(ast::node_id, const ast::block_stmt&) -> void;
    auto visit(ast::node_id, const ast::break_stmt&) -> void;
    auto visit(ast::node_id, const ast::cfg_stmt&) -> void;
    auto visit(ast::node_id, const ast::continue_stmt&) -> void;
    auto visit(ast::node_id, const ast::decl_stmt&) -> void;
    auto visit(ast::node_id, const ast::defer_stmt&) -> void;
    auto visit(ast::node_id, const ast::discard_stmt&) -> void;
    auto visit(ast::node_id, const ast::expr_stmt&) -> void;

    [[nodiscard]] auto collect_import_payload(const ast::import_stmt& import_stmt)
        -> std::pair<std::string_view, stdx::result<gsl::not_null<mod::module*>, mod::diagnostic>>;

    auto visit(ast::node_id, const ast::import_stmt&) -> void;
    auto visit(ast::node_id, const ast::return_stmt&) -> void;
    auto visit(ast::node_id, const ast::test_stmt&) -> void;
    auto visit(ast::node_id, const ast::using_stmt&) -> void;

    auto visit(ast::explicit_type_id, const ast::identifier_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::module_access_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::dot_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::call_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::explicit_function_type&) -> void;
    auto visit(ast::explicit_type_id, ast::explicit_type_id id) -> void { collect(id); }
    auto visit(ast::explicit_type_id, const ast::explicit_array_type&) -> void;

    template <typename... IterPairs>
    [[nodiscard]] auto visit_scopes(type_kind kind, IterPairs&&... pairs) -> usize {
        const auto  new_idx{ctx_.registry.create()};
        const scope s{table_stack_, new_idx, table_idx_};
        (..., [&pairs] -> void {
            for (const auto& item : pairs.iterable) { pairs.visitor(item); }
        }());
        last_type_.emplace(ctx_.pool[{kind, types::mut::CONSTANT, new_idx}]);
        return new_idx;
    }

    template <typename SymbolicVariant, typename... Args>
    auto try_declare(std::string_view name, Args&&... args) -> bool {
        const SymbolicVariant node{std::forward<Args>(args)...};
        return ctx_.try_result(ctx_.registry.is_shadowing(table_stack_, collecting_, name, node)) &&
               ctx_.try_result(ctx_.registry.insert_into(table_idx_, collecting_, name, node));
    }

    symbol_collector(mod::module& collecting, context& ctx)
        : collecting_{collecting}, table_idx_{*collecting.root_table_idx}, ctx_{ctx} {
        table_stack_.push(table_idx_);
    }

  private:
    mod::module&        collecting_;
    usize               table_idx_;
    symbol_table_stack  table_stack_;
    context&            ctx_;
    stdx::option<type&> last_type_;

    default_counter in_expr_scope_;
    default_counter in_function_scope_;
    default_counter in_loop_scope_;
    default_counter in_label_scope_;
    default_counter in_test_scope_;
};

} // namespace ghoti::sema
