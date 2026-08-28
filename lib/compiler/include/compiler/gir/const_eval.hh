#pragma once

#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"

namespace ghoti::gir {

class const_eval {
  public:
    explicit const_eval(sema::context& ctx, mod::module& module) noexcept
        : ctx_{ctx}, module_{&module} {}
    ~const_eval() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(const_eval);

    auto set_module(mod::module& module) noexcept -> void { module_ = &module; }

    // Attempt to evaluate node as a compile-time constant. Returns none if non-constant.
    [[nodiscard]] auto try_eval(ast::node_id id) -> stdx::option<const_value>;

    // Evaluate and assert. Emits CONSTEXPR_EVALUATION_FAILED and returns poison on failure.
    [[nodiscard]] auto eval(ast::node_id id) -> const_value;

    // Evaluates an expression as a non-negative integer dimension for array sizing
    [[nodiscard]] auto eval_type_dim(ast::node_id id) -> stdx::option<usize>;

    auto resolve_all_deferred_arrays() -> void;
    auto resolve_all_deferred_calls() -> void;
    auto resolve_all_deferred_types() -> void;

    // Forces a single possibly-deferred array type to its concrete resolved form
    [[nodiscard]] auto force_deferred_array(sema::type& maybe_deferred) -> sema::type&;

    [[nodiscard]] static auto type_align_of(const sema::type& type, usize ptr_size) -> usize;
    [[nodiscard]] static auto type_size_of(const sema::type& type, usize ptr_size) -> usize;

  private:
    struct call_frame {
        ankerl::unordered_dense::map<std::string_view, const_value> bindings;
    };

  private:
    auto resolve_deferred_array(const ast::explicit_array_type& array, sema::type& item_type)
        -> sema::type&;
    auto force_deferred_function_params(sema::type& maybe_fn) -> void;
    auto force_deferred_aggregate_fields(sema::type& maybe_aggregate) -> void;
    auto force_deferred_indirection_underlying(sema::type& maybe_indirection) -> void;
    auto force_deferred_array_elements(gsl::span<sema::type*> elements) -> void;
    auto resolve_deferred_call(const ast::call_expr& call) -> sema::type&;

    auto eval_node(ast::node_id id) -> stdx::option<const_value>;
    auto eval_binary(ast::node_id id, const ast::binary_expr& binary) -> stdx::option<const_value>;
    auto eval_assignment(ast::node_id                id,
                         const ast::assignment_expr& assign,
                         syntax::token_type_t        op_type) -> stdx::option<const_value>;
    auto fold_binary_values(syntax::token_type_t op_type,
                            const const_value&   lhs,
                            const const_value&   rhs,
                            ast::node_id         id) -> stdx::option<const_value>;
    auto eval_unary(ast::node_id id, const ast::unary_expr& unary) -> stdx::option<const_value>;
    auto eval_ident(ast::node_id id, const ast::identifier_expr& ident)
        -> stdx::option<const_value>;
    auto eval_call(ast::node_id id, const ast::call_expr& call) -> stdx::option<const_value>;
    auto eval_builtin(const ast::call_expr& call, syntax::token_type_t builtin_type)
        -> stdx::option<const_value>;
    auto eval_constexpr_fn(ast::node_id                    call_id,
                           const ast::function_expr&       fn_expr,
                           const std::vector<const_value>& args) -> stdx::option<const_value>;

    auto eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value>;
    auto eval_decl(ast::node_id id, const ast::decl_stmt& decl) -> stdx::option<const_value>;
    auto eval_block(ast::node_id id, const ast::block_stmt& block) -> stdx::option<const_value>;
    auto eval_if(ast::node_id id, const ast::if_expr& if_expr) -> stdx::option<const_value>;
    auto eval_while(ast::node_id id, const ast::while_loop_expr& loop) -> stdx::option<const_value>;
    auto eval_do_while(ast::node_id id, const ast::do_while_loop_expr& loop)
        -> stdx::option<const_value>;
    auto eval_for(ast::node_id id, const ast::for_loop_expr& loop) -> stdx::option<const_value>;

    auto eval_array(ast::node_id id, const ast::array_expr& array) -> stdx::option<const_value>;
    auto eval_index(ast::node_id id, const ast::index_expr& index_expr)
        -> stdx::option<const_value>;
    auto eval_initializer(ast::node_id id, const ast::initializer_expr& init)
        -> stdx::option<const_value>;
    auto eval_dot(ast::node_id id, const ast::dot_expr& dot) -> stdx::option<const_value>;
    auto eval_implicit_access(ast::node_id id, const ast::implicit_access_expr& implicit)
        -> stdx::option<const_value>;
    auto eval_module_access(ast::node_id id, const ast::module_access_expr& mod_access)
        -> stdx::option<const_value>;
    auto eval_match(ast::node_id id, const ast::match_expr& match) -> stdx::option<const_value>;
    auto match_pattern(const ast::match_pattern_handle& pattern_h, const const_value& target)
        -> bool;

    auto lookup_local_binding(std::string_view name) const noexcept -> stdx::option<const_value>;
    auto mutate_local_binding(std::string_view name, const_value val) -> bool;

  private:
    usize                                            max_recursion_depth_{256};
    std::vector<usize>                               recursion_limit_stack_;
    sema::context&                                   ctx_;
    gsl::not_null<mod::module*>                      module_;
    std::vector<call_frame>                          call_stack_;
    default_counter                                  recursion_depth_;
    ankerl::unordered_dense::map<usize, const_value> memo_cache_;
};

} // namespace ghoti::gir
