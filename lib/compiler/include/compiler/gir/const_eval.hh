#pragma once

#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
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

namespace ghoti::gir {

class const_eval {
  public:
    static constexpr usize MAX_RECURSION_DEPTH{256};

  public:
    explicit const_eval(sema::context& ctx, mod::module& module) noexcept
        : ctx_{ctx}, module_{module} {}
    ~const_eval() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(const_eval);

    // Attempt to evaluate node as a compile-time constant. Returns none if non-constant.
    [[nodiscard]] auto try_eval(ast::node_id id) -> stdx::option<const_value>;

    // Evaluate and assert. Emits CONSTEXPR_EVALUATION_FAILED and returns poison on failure.
    [[nodiscard]] auto eval(ast::node_id id) -> const_value;

    // Evaluates an expression as a non-negative integer dimension for array sizing
    [[nodiscard]] auto eval_type_dim(ast::node_id id) -> stdx::option<usize>;

    auto resolve_deferred_array(ast::explicit_type_id           id,
                                const ast::explicit_array_type& array,
                                sema::type&                     item_type) -> sema::type&;
    auto resolve_all_deferred_arrays() -> void;

    [[nodiscard]] static auto type_size_of(const sema::type& type) -> usize;
    [[nodiscard]] static auto type_align_of(const sema::type& type) -> usize;

  private:
    struct call_frame {
        ankerl::unordered_dense::map<std::string_view, const_value> bindings;
    };

    auto eval_node(ast::node_id id) -> stdx::option<const_value>;
    auto eval_binary(ast::node_id id, const ast::binary_expr& binary) -> stdx::option<const_value>;
    auto eval_unary(ast::node_id id, const ast::unary_expr& unary) -> stdx::option<const_value>;
    auto eval_ident(ast::node_id id, const ast::identifier_expr& ident)
        -> stdx::option<const_value>;
    auto eval_call(ast::node_id id, const ast::call_expr& call) -> stdx::option<const_value>;
    auto eval_builtin(ast::node_id          id,
                      const ast::call_expr& call,
                      syntax::token_type_t  builtin_type) -> stdx::option<const_value>;
    auto eval_constexpr_fn(ast::node_id                    call_id,
                           const sema::symbol&             sym,
                           const ast::function_expr&       fn_expr,
                           const std::vector<const_value>& args) -> stdx::option<const_value>;

    auto eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value>;
    auto eval_decl(ast::node_id id, const ast::decl_stmt& decl) -> stdx::option<const_value>;
    auto eval_block(ast::node_id id, const ast::block_stmt& block) -> stdx::option<const_value>;
    auto eval_if(ast::node_id id, const ast::if_expr& if_expr) -> stdx::option<const_value>;

    auto lookup_local_binding(std::string_view name) const noexcept -> stdx::option<const_value>;

  private:
    sema::context&                                   ctx_;
    mod::module&                                     module_;
    std::vector<call_frame>                          call_stack_;
    usize                                            recursion_depth_{0};
    ankerl::unordered_dense::map<usize, const_value> memo_cache_;
};

} // namespace ghoti::gir
