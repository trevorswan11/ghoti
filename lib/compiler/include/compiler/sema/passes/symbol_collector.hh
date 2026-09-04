#pragma once

#include <string_view>
#include <utility>
#include <vector>

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
    template <ast::IndexableID ID> auto visit(ID, const ast::interface_expr&) -> void;
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
    auto visit(ast::node_id, const ast::impl_stmt&) -> void;

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
    auto visit(ast::explicit_type_id, const ast::explicit_dyn_type&) -> void;

    template <typename... IterPairs>
    [[nodiscard]] auto visit_scopes(type_kind kind, IterPairs&&... pairs) -> usize {
        const auto  new_idx{ctx_.registry.create()};
        const scope s{table_stack_, new_idx, table_idx_};

        // A struct/union/enum/interface body is its own namespace
        const auto is_agg{is_aggregate(kind)};
        if (is_agg) {
            // A member may legitimately share a name with a declaration in an enclosing scope
            aggregate_table_stack_.emplace_back(new_idx, std::exchange(pending_type_name_, {}));
        }

        const auto agg_cleanup{gsl::finally([this, is_agg] {
            if (is_agg) { aggregate_table_stack_.pop_back(); }
        })};

        (..., [&pairs] -> void {
            for (const auto& item : pairs.iterable) { pairs.visitor(item); }
        }());

        if (is_agg) {}
        last_type_.emplace(ctx_.pool[{kind, types::mut::CONSTANT, new_idx}]);
        return new_idx;
    }

    // Whether a name declared right now would land directly in a struct/union/enum body
    [[nodiscard]] auto declaring_into_aggregate() const -> bool {
        return !aggregate_table_stack_.empty() && aggregate_table_stack_.back().first == table_idx_;
    }

    template <typename SymbolicVariant, typename... Args>
    auto try_declare(std::string_view name, Args&&... args) -> bool {
        const SymbolicVariant node{std::forward<Args>(args)...};

        // Names declared directly into an aggregate's own table are namespaced by that type
        const bool into_aggregate{declaring_into_aggregate()};
        const bool shadows_own_type{into_aggregate &&
                                    !aggregate_table_stack_.back().second.empty() &&
                                    name == aggregate_table_stack_.back().second};
        if ((!into_aggregate || shadows_own_type) &&
            !ctx_.try_result(ctx_.registry.is_shadowing(table_stack_, collecting_, name, node))) {
            return false;
        }
        return ctx_.try_result(ctx_.registry.insert_into(table_idx_, collecting_, name, node));
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

    // The struct/union/enum tables currently being populated (innermost last)
    std::vector<std::pair<usize, std::string_view>> aggregate_table_stack_;

    // Set by `visit(decl_stmt)` immediately before descending into an aggregate value
    std::string_view pending_type_name_;
};

} // namespace ghoti::sema
