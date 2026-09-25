#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/symbol_scoping.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"

namespace ghoti::gir {

class const_eval {
  public:
    struct constexpr_context_guard {
        explicit constexpr_context_guard(const_eval& ce, bool enabled = true) noexcept
            : ce_{ce}, prev_{ce.constexpr_context_} {
            ce_.constexpr_context_ = enabled;
        }
        ~constexpr_context_guard() noexcept { ce_.constexpr_context_ = prev_; }
        MAKE_PINNED(constexpr_context_guard);

      private:
        const_eval& ce_;
        bool        prev_;
    };

  public:
    explicit const_eval(sema::context& ctx, mod::module& module) noexcept
        : ctx_{ctx}, module_{&module} {}
    ~const_eval() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(const_eval);

    auto set_module(mod::module& module) noexcept -> void {
        if (module_ != &module) { clear_memo(); }
        module_ = &module;
    }

    // The struct/union/enum whose member body is currently being evaluated
    auto set_enclosing_type(stdx::option<sema::type&> type) noexcept -> void {
        if (enclosing_type_ != type) { clear_memo(); }
        enclosing_type_ = type;
    }

    // The emitter's program-wide symbol-naming policy
    auto set_symbol_scoping(stdx::option<const symbol_scoping&> scoping) noexcept -> void {
        symbol_scoping_ = scoping;
    }

    // The module `coerce_dyn` registers a synthesized `__vtable.<N>` global onto
    auto set_vtable_root_module(mod::module& root) noexcept -> void { vtable_root_ = root; }

    [[nodiscard]] auto scoped_symbol_name(usize owner_table_idx, std::string_view bare) const
        -> std::string {
        if (symbol_scoping_) { return symbol_scoping_->name_for(owner_table_idx, bare); }
        return std::string{bare};
    }

    // Node-indexed memoization is unsound across generic instantiations that share AST nodes.
    auto clear_memo() noexcept -> void {
        memo_cache_.clear();
        ctx_.advance_epoch();
    }

    // Attempt to evaluate node as a compile-time constant. Returns none if non-constant.
    [[nodiscard]] auto try_eval(ast::node_id id) -> stdx::option<const_value>;

    // Folds two already-evaluated constants under `op_type` (e.g. for a `+=` on a folded local).
    [[nodiscard]] auto fold_binary_values(syntax::token_type_t op_type,
                                          const const_value&   lhs,
                                          const const_value&   rhs,
                                          ast::node_id         id) -> stdx::option<const_value>;

    [[nodiscard]] auto arm_pattern_matches(const ast::match_pattern_handle& pattern,
                                           const const_value&               target) -> bool {
        return match_pattern(pattern, target);
    }

    // Evaluate and assert. Emits CONSTEXPR_EVALUATION_FAILED and returns poison on failure.
    [[nodiscard]] auto eval(ast::node_id id) -> const_value;

    // Evaluates an expression as a non-negative integer dimension for array sizing
    [[nodiscard]] auto eval_type_dim(ast::node_id id) -> stdx::option<usize>;

    auto resolve_all_deferred_arrays() -> void;
    auto resolve_all_deferred_calls() -> void;
    auto resolve_all_deferred_types() -> void;

    // Forces a single possibly-deferred array type to its concrete resolved form
    [[nodiscard]] auto force_deferred_array(sema::type& maybe_deferred) -> sema::type&;

    // As above, and also through pointer, reference, slice, and function signature types
    [[nodiscard]] auto force_deferred_type(sema::type& maybe_deferred) -> sema::type&;

    // Forces a `fn(...): type` deferred-call type to the type it produces
    [[nodiscard]] auto force_deferred_call(sema::type& maybe_deferred) -> sema::type&;

    [[nodiscard]] static auto type_align_of(const sema::type& type, usize ptr_size) -> usize;
    [[nodiscard]] static auto type_size_of(const sema::type& type, usize ptr_size) -> usize;
    // The type an unannotated type-alias decl (`const X := T;`) names, read off its own node so a
    // generic instantiation's body overlay stays per-instantiation
    [[nodiscard]] static auto
    alias_decl_type(const mod::module& mod, ast::node_id node, const ast::decl_stmt& decl)
        -> stdx::option<sema::type&>;

    [[nodiscard]] auto coerce_dyn(const const_value& val, sema::type& dest_type)
        -> stdx::option<const_value>;

    /// Simulates the execution of preceding statements across active lexical blocks
    /// up to each frame's `current_stmt_idx` for `constexpr var` observability
    auto simulate_active_blocks(gsl::span<const sema::active_block_frame> blocks,
                                sema::constexpr_frame&                    out_frame) -> void;

    [[nodiscard]] auto is_constexpr_context() const noexcept -> bool {
        return recursion_depth_ > 0 || constexpr_context_;
    }

    auto set_constexpr_context(bool enabled) noexcept -> void { constexpr_context_ = enabled; }

  private:
    struct defer_entry {
        ast::stmt_handle                            stmt;
        bool                                        is_errdefer{false};
        stdx::option<ast::discardable_ident_handle> capture;
        ast::type_modifier                          modifier;
    };

    struct call_frame {
        ankerl::unordered_dense::map<std::string_view, const_value> bindings;
        std::vector<defer_entry>                                    defers;
    };

    // A `constexpr` callable (closure or plain function) bound to `name` in scope
    struct bound_callable {
        gsl::not_null<mod::module*>              module;
        gsl::not_null<const ast::function_expr*> fn_expr;
        stdx::option<const const_struct&>        captures{};
    };

    struct memo_key {
        usize node_index{};
        u64   epoch{};

        [[nodiscard]] auto operator==(const memo_key& other) const noexcept -> bool = default;
    };

    struct memo_key_hash {
        [[nodiscard]] static constexpr auto operator()(const memo_key& k) noexcept -> u64 {
            return stdx::hasher{k.node_index}.combine(k.epoch).finalize();
        }
    };

    enum class eval_signal_kind : u8 {
        RETURN,
        BREAK,
        CONTINUE,
    };

    struct eval_signal {
        stdx::option<eval_signal_kind> kind{};
        stdx::option<std::string_view> target_label{};
        stdx::option<const_value>      value{};
    };

  private:
    [[nodiscard]] auto resolve_deferred_array(const sema::types::deferred_array& deferred)
        -> stdx::option<sema::type&>;
    [[nodiscard]] auto eval_slice_copy(ast::node_id id, ast::node_id slice_expr)
        -> stdx::option<const_value>;
    auto write_range_target(const ast::index_expr& target,
                            const ast::range_expr& range,
                            const_value            container,
                            const_value            val) -> bool;
    auto force_deferred_function_params(sema::type& maybe_fn) -> void;
    auto force_deferred_aggregate_fields(sema::type& maybe_aggregate) -> void;
    auto force_deferred_indirection_underlying(sema::type& maybe_indirection) -> void;
    auto force_deferred_array_elements(gsl::span<sema::type*> elements) -> void;
    // Deep-forces array placeholders reachable by value; `none` if any stays deferred
    [[nodiscard]] auto force_deferred_layout(sema::type& type) -> stdx::option<sema::type&>;
    [[nodiscard]] auto rebuild_array(sema::type& array_type, sema::type& underlying) -> sema::type&;
    auto               resolve_deferred_call(const ast::call_expr& call) -> sema::type&;
    // As above but yields `none` instead of a diagnostic when the call cannot be evaluated yet.
    [[nodiscard]] auto try_resolve_deferred_call(const ast::call_expr& call)
        -> stdx::option<sema::type&>;

    auto eval_node(ast::node_id id) -> stdx::option<const_value>;
    auto eval_binary(ast::node_id id, const ast::binary_expr& binary) -> stdx::option<const_value>;
    auto eval_assignment(ast::node_id                id,
                         const ast::assignment_expr& assign,
                         syntax::token_type_t        op_type) -> stdx::option<const_value>;
    // `lhs ++ rhs`: concatenates two array/slice/string constants; result type from `id`.
    auto fold_concat(const const_value& lhs, const const_value& rhs, ast::node_id id)
        -> stdx::option<const_value>;
    auto eval_unary(ast::node_id id, const ast::unary_expr& unary) -> stdx::option<const_value>;
    auto eval_address_of(ast::node_id id, ast::node_id rhs) -> stdx::option<const_value>;
    auto eval_ident(ast::node_id id, const ast::identifier_expr& ident)
        -> stdx::option<const_value>;
    auto eval_call(ast::node_id id, const ast::call_expr& call) -> stdx::option<const_value>;
    auto eval_builtin(ast::node_id          id,
                      const ast::call_expr& call,
                      syntax::token_type_t  builtin_type) -> stdx::option<const_value>;

    [[nodiscard]] auto target_enum_value(std::string_view enum_name, std::string_view member)
        -> const_value;

    // `@typeInfo(T)`: builds the `builtin::TypeInfo` tagged union for `denoted` (already
    // unwrapped past any `TYPE`/`deferred_call` wrapper) by switching on its `type_kind`.
    [[nodiscard]] auto eval_type_info(sema::type& denoted) -> const_value;
    auto               eval_constexpr_fn(ast::node_id                      call_id,
                                         const ast::function_expr&         fn_expr,
                                         std::vector<const_value>&         args,
                                         stdx::option<const const_struct&> captures = stdx::none)
        -> stdx::option<const_value>;

    auto lookup_bound_callable(std::string_view name) -> stdx::option<bound_callable>;

    auto eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value>;
    auto eval_decl(ast::node_id id, const ast::decl_stmt& decl) -> stdx::option<const_value>;
    auto eval_block(ast::node_id id, const ast::block_stmt& block) -> stdx::option<const_value>;
    auto eval_label(ast::node_id id, const ast::label_expr& label) -> stdx::option<const_value>;
    auto eval_if(ast::node_id id, const ast::if_expr& if_expr) -> stdx::option<const_value>;
    auto eval_while(ast::node_id id, const ast::while_loop_expr& loop) -> stdx::option<const_value>;
    auto eval_do_while(ast::node_id id, const ast::do_while_loop_expr& loop)
        -> stdx::option<const_value>;
    auto eval_infinite_loop(ast::node_id id, const ast::infinite_loop_expr& loop)
        -> stdx::option<const_value>;
    auto eval_for(ast::node_id id, const ast::for_loop_expr& loop) -> stdx::option<const_value>;

    auto eval_array(ast::node_id id, const ast::array_expr& array) -> stdx::option<const_value>;
    auto eval_index(ast::node_id id, const ast::index_expr& index_expr)
        -> stdx::option<const_value>;
    // `arr[lo..hi]` / `arr[lo..=hi]`: a compile-time sub-array/sub-string slice value.
    auto eval_slice_index(ast::node_id           id,
                          const const_value&     target_val,
                          ast::node_id           range_id,
                          const ast::range_expr& range) -> stdx::option<const_value>;
    auto eval_initializer(ast::node_id id, const ast::initializer_expr& init)
        -> stdx::option<const_value>;
    auto eval_dot(ast::node_id id, const ast::dot_expr& dot) -> stdx::option<const_value>;
    auto eval_implicit_access(ast::node_id id, const ast::implicit_access_expr& implicit)
        -> stdx::option<const_value>;

    // Resolves a (possibly chained) module operand to the imported module it names
    auto resolve_module_chain(ast::node_id node) -> stdx::option<mod::module&>;

    // Evaluates `member`, looked up in `target_mod`'s root scope, as a cross-module constant.
    auto eval_module_member(mod::module& target_mod, std::string_view member)
        -> stdx::option<const_value>;

    // Resolves `member` against an already-denoted aggregate type.
    // Shared by `Type.member` and `alias::Type::member`.
    auto eval_type_member(sema::type& denoted, std::string_view member)
        -> stdx::option<const_value>;
    auto eval_match(ast::node_id id, const ast::match_expr& match) -> stdx::option<const_value>;
    auto eval_unwrap(ast::node_id id, const ast::unwrap_expr& unwrap) -> stdx::option<const_value>;
    auto match_pattern(const ast::match_pattern_handle& pattern_h, const const_value& target)
        -> bool;

    auto lookup_local_binding(std::string_view name) const noexcept -> stdx::option<const_value>;
    auto set_local_binding(std::string_view name, const_value val) -> bool;
    auto write_target(ast::node_id target, const_value val) -> bool;

    // Simulates execution of a statement preceding the current evaluation site.
    // Dispatches declarations, expressions/assignments, sub-blocks, and control-flow signals.
    auto simulate_stmt(const ast::stmt_handle& stmt) -> void;
    auto simulate_expr(ast::node_id id) -> void;

    // Bails if the decl does not have a constexpr modifier
    auto simulate_decl(const ast::decl_stmt& decl) -> void;
    auto simulate_assignment(ast::node_id                id,
                             const ast::assignment_expr& assign,
                             syntax::token_type_t        op) -> void;
    auto simulate_if(const ast::if_expr& if_expr) -> void;
    auto simulate_while(const ast::while_loop_expr& loop) -> void;
    auto simulate_do_while(const ast::do_while_loop_expr& loop) -> void;
    auto simulate_infinite_loop(const ast::infinite_loop_expr& loop) -> void;
    auto simulate_for(const ast::for_loop_expr& loop) -> void;
    auto simulate_block(const ast::block_stmt& block) -> void;

  private:
    // Names of mutable `constexpr var` bindings currently in scope during simulation.
    ankerl::unordered_dense::set<std::string_view> active_cx_vars_;
    usize                                          max_recursion_depth_{256};
    std::vector<usize>                             recursion_limit_stack_;
    sema::context&                                 ctx_;
    gsl::not_null<mod::module*>                    module_;
    stdx::option<mod::module&>                     vtable_root_;
    stdx::option<sema::type&>                      enclosing_type_;
    stdx::option<const symbol_scoping&>            symbol_scoping_;
    std::vector<call_frame>                        call_stack_;
    default_counter                                recursion_depth_;

    // Set by `eval_if`/`eval_while`/`eval_do_while`/`eval_for` when a construct's own
    // condition/iterable can't be folded.
    bool                      cond_unknown_{false};
    bool                      constexpr_context_{false};
    eval_signal               current_signal_{};
    stdx::option<const_value> current_error_val_{};

    ankerl::unordered_dense::map<memo_key, const_value, memo_key_hash> memo_cache_;
    ankerl::unordered_dense::map<std::string, const_value>             global_cx_vars_;
};

} // namespace ghoti::gir
