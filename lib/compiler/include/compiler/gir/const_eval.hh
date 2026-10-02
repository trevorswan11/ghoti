#pragma once

#include <deque>
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
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"
#include "support/int128.hh"

namespace ghoti::gir {

class const_eval {
  public:
    struct comptime_context_guard {
        explicit comptime_context_guard(const_eval& ce, bool enabled = true) noexcept
            : ce_{ce}, prev_{ce.comptime_context_} {
            ce_.comptime_context_ = enabled;
        }
        ~comptime_context_guard() noexcept { ce_.comptime_context_ = prev_; }
        MAKE_PINNED(comptime_context_guard);

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

    // Attempt to evaluate node as a compile-time constant. Returns none if non-constant. A
    // reference or slice that views a compile-time place is read through, as runtime loads it
    [[nodiscard]] auto try_eval(ast::node_id id) -> stdx::option<const_value>;

    // Folds two already-evaluated constants under `op_type` (e.g. for a `+=` on a folded local).
    [[nodiscard]] auto fold_binary_values(syntax::token_type_t op_type,
                                          const const_value&   lhs,
                                          const const_value&   rhs,
                                          ast::node_id         id) -> stdx::option<const_value>;

    // Folds two integers exactly: a concrete signed type rejects overflow unless `wrapping`, and
    // unsigned results (and shifts) wrap to the type's width as they do at runtime
    // `@abs`, `@clz`, `@ctz`, and `@popCount` of a concrete integer, or none for anything else
    [[nodiscard]] auto fold_int_builtin_unary(ast::node_id         id,
                                              syntax::token_type_t builtin,
                                              const const_value& arg) -> stdx::option<const_value>;
    [[nodiscard]] auto fold_integer_binary(syntax::token_type_t op_type,
                                           const const_value&   lhs,
                                           const const_value&   rhs,
                                           ast::node_id         id,
                                           bool wrapping = false) -> stdx::option<const_value>;

    [[nodiscard]] auto arm_pattern_matches(const ast::match_pattern_handle& pattern,
                                           const const_value&               target) -> bool {
        return match_pattern(pattern, target);
    }

    // Evaluate and assert. Emits COMPTIME_EVALUATION_FAILED and returns poison on failure.
    [[nodiscard]] auto eval(ast::node_id id) -> const_value;

    // Evaluates an expression as a non-negative integer dimension for array sizing
    [[nodiscard]] auto eval_type_dim(ast::node_id id) -> stdx::option<usize>;

    auto resolve_all_deferred_arrays() -> void;
    auto resolve_all_deferred_calls() -> void;
    auto resolve_all_deferred_types() -> void;

    // Forces a single possibly-deferred array type to its concrete resolved form
    [[nodiscard]] auto force_deferred_array(sema::type& maybe_deferred) -> sema::type&;

    // Every member's discriminant in declaration order (an unvalued member is its predecessor + 1)
    [[nodiscard]] static auto is_sentinel_terminated(stdx::option<sema::type&> type) noexcept
        -> bool;
    [[nodiscard]] auto enum_member_values(const sema::types::enum_t& en) -> std::vector<i128>;
    [[nodiscard]] auto enum_member_value(const sema::types::enum_t& en, std::string_view member)
        -> stdx::option<i128>;

    // As above, and also through pointer, reference, slice, and function signature types
    [[nodiscard]] auto force_deferred_type(sema::type& maybe_deferred) -> sema::type&;

    // The type a local of the call being evaluated was annotated with, when the annotation names
    // one of the call's type parameters (`let mut a: T`); the shared body's typing only knows `T`
    [[nodiscard]] auto call_local_annotation(ast::node_id use, const ast::identifier_expr& ident)
        -> stdx::option<sema::type&>;

    // Forces a `fn(...): type` deferred-call type to the type it produces
    [[nodiscard]] auto force_deferred_call(sema::type& maybe_deferred) -> sema::type&;

    [[nodiscard]] static auto type_align_of(const sema::type& type, usize ptr_size) -> usize;
    // A struct field's alignment: its type's, raised by any `@[align(n)]` on the field
    [[nodiscard]] static auto
    struct_field_align(const sema::types::struct_t& st, usize idx, usize ptr_size) -> usize;
    [[nodiscard]] static auto type_size_of(const sema::type& type, usize ptr_size) -> usize;
    // The type an unannotated type-alias decl (`const X = T;`) names, read off its own node so a
    // generic instantiation's body overlay stays per-instantiation
    [[nodiscard]] static auto
    alias_decl_type(const mod::module& mod, ast::node_id node, const ast::decl_stmt& decl)
        -> stdx::option<sema::type&>;

    [[nodiscard]] auto coerce_dyn(const const_value& val, sema::type& dest_type)
        -> stdx::option<const_value>;

    /// Simulates the execution of preceding statements across active lexical blocks
    /// up to each frame's `current_stmt_idx` for `comptime let mut` observability
    auto simulate_active_blocks(gsl::span<const sema::active_block_frame> blocks,
                                sema::comptime_frame&                     out_frame) -> void;

    [[nodiscard]] auto is_comptime_context() const noexcept -> bool {
        return recursion_depth_ > 0 || comptime_context_;
    }

    // Whether a compile-time context asked for this evaluation, as opposed to an opportunistic
    // fold of runtime code; selects the arm of a condition-less `if comptime`
    [[nodiscard]] auto in_evaluation_context() const noexcept -> bool {
        return comptime_context_ || ctx_.comptime_evaluation_depth > 0;
    }

    auto set_comptime_context(bool enabled) noexcept -> void { comptime_context_ = enabled; }

    // What `branch(&operand)` of an `Unwrappable` operand returned: whether it was the `break`
    // variant, its payload (a reference for `continue`, the residual for `break`), and the
    // operand's own value
    struct flow_eval {
        bool        is_break{false};
        const_value payload;
        const_value operand;
    };

    // `eval_flow` from outside any evaluation, with a `continue` payload read by value
    [[nodiscard]] auto try_eval_flow(ast::expr_handle operand, ast::node_id at)
        -> stdx::option<flow_eval>;

    // The arm a folded matcher selects; none when the matcher does not fold or nothing matches
    [[nodiscard]] auto selected_match_arm(const ast::match_expr& match) -> stdx::opt_size;

    // The folded attributes of the function declaration `arg` names, as `f` or as `T.f`, seen
    // through the instantiation that produced `T` when a type constructor did
    [[nodiscard]] auto declared_fn_attributes(ast::expr_handle arg)
        -> stdx::option<sema::resolved_attributes>;

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
        // Unique per pushed frame, so a `const_ref` into a popped frame is never followed
        u64   id{0};
        usize temporaries{0};
    };

    // A `comptime` callable (closure or plain function) bound to `name` in scope
    struct bound_callable {
        gsl::not_null<mod::module*>              module;
        gsl::not_null<const ast::function_expr*> fn_expr;
        stdx::option<const const_struct&>        captures{};
    };

    struct memo_key {
        usize node_index{};
        u64   epoch{};
        bool  evaluation_context{};

        [[nodiscard]] auto operator==(const memo_key& other) const noexcept -> bool = default;
    };

    struct memo_key_hash {
        [[nodiscard]] static constexpr auto operator()(const memo_key& k) noexcept -> u64 {
            return stdx::hasher{k.node_index}
                .combine(k.epoch)
                .combine(k.evaluation_context)
                .finalize();
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

    // What a loop does after its body ran
    enum class loop_step : u8 {
        NEXT, // iterate again
        STOP, // stop (unlabeled `break`)
        EXIT, // exit with `current_signal_`
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

    // Whether `id`'s value is a reference or slice, which a branch hands on without reading
    [[nodiscard]] auto yields_view(ast::node_id id) const -> bool;
    // `try_eval` without reading through a reference or slice to a compile-time place
    [[nodiscard]] auto try_eval_raw(ast::node_id id) -> stdx::option<const_value>;
    // The value `id` passes to a destination of type `dest`: a reference or slice destination
    // keeps (or takes) a reference to the place `id` names
    [[nodiscard]] auto try_eval_for(ast::node_id id, stdx::option<sema::type&> dest)
        -> stdx::option<const_value>;

    auto push_frame() -> call_frame&;
    auto push_frame(call_frame frame) -> call_frame&;
    // The binding a `const_ref` is rooted at, or none once its frame is gone
    [[nodiscard]] auto ref_root(const const_ref& ref) -> stdx::option<const_value&>;
    // A reference to the compile-time place `id` names (a binding, or a field, payload or element
    // of one), or none when it names no place a frame of this evaluation owns
    [[nodiscard]] auto eval_place(ast::node_id id) -> stdx::option<const_ref>;
    // `id`'s value when it is a reference or pointer to a compile-time place
    [[nodiscard]] auto raw_ref_of(ast::node_id id) -> stdx::option<const_ref>;
    [[nodiscard]] auto load_ref(const const_ref& ref, ast::node_id at) -> stdx::option<const_value>;
    [[nodiscard]] auto store_ref(const const_ref& ref, const_value val) -> bool;
    // A reference into a temporary: binds `val` to a hidden slot of the current frame
    [[nodiscard]] auto ref_to_temporary(const_value val) -> const_ref;
    // Reports a reference into a frame at or above `first_dead` that survives that frame's exit
    [[nodiscard]] auto reject_escaping_ref(const const_value& val, u64 first_dead, ast::node_id at)
        -> bool;

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

    [[nodiscard]] auto target_pointer_bits() const -> u32;
    [[nodiscard]] auto target_pointer_bytes() const -> usize;
    // The value operand of a cast-style builtin: `@cast(T, x)` or `@cast(x)`
    [[nodiscard]] static auto cast_operand(const ast::call_expr& call)
        -> stdx::option<ast::expr_handle>;
    // A builtin call's resolved type, falling back to the type recorded on its callee
    [[nodiscard]] auto builtin_result_type(ast::node_id id, const ast::call_expr& call) const
        -> stdx::option<sema::type&>;

    // `@typeInfo(T)`: builds the `builtin::TypeInfo` tagged union for `denoted` (already
    // unwrapped past any `TYPE`/`deferred_call` wrapper) by switching on its `type_kind`.
    [[nodiscard]] auto member_fn_attributes(const ast::dot_expr& dot)
        -> stdx::option<sema::resolved_attributes>;
    [[nodiscard]] auto
    eval_type_info(sema::type&                                    denoted,
                   stdx::option<const sema::resolved_attributes&> declared = stdx::none)
        -> const_value;
    // An `if`'s folded condition; `if comptime { ... }` is true only in a comptime context
    auto eval_if_condition(const ast::if_expr& if_expr) -> stdx::option<const_value>;
    // Folds a declaration's initializer; a `const` one is always compile-time evaluation
    // Whether a type annotation names a `comptime let mut` binding, whose value changes
    [[nodiscard]] auto names_comptime_mut(ast::explicit_type_id annotation) const -> bool;
    auto               eval_decl_value(const ast::decl_stmt& decl) -> stdx::option<const_value>;
    // `= undefined` for `declared` under compile-time evaluation: an aggregate gets its full
    // shape with every leaf undefined, so elements and fields can be written one at a time
    auto undefined_value_of(sema::type& declared) -> const_value;
    // A plain call's parameter types, empty when the callee's type isn't a known function
    [[nodiscard]] auto callee_params(const ast::call_expr& call) -> gsl::span<sema::type*>;
    // Evaluates a call's arguments, splicing each `rest...` expansion's elements into place
    auto eval_call_args(const ast::call_expr& call) -> stdx::option<std::vector<const_value>>;
    auto eval_comptime_fn(ast::node_id                      call_id,
                          const ast::function_expr&         fn_expr,
                          std::vector<const_value>&         args,
                          stdx::option<const const_struct&> captures = stdx::none)
        -> stdx::option<const_value>;

    auto lookup_bound_callable(std::string_view name) -> stdx::option<bound_callable>;

    auto eval_stmt(const ast::stmt_handle& stmt) -> stdx::option<const_value>;
    // A loop's `else` branch, whose expression (if it is one) is the loop's value
    auto eval_non_break(const ast::stmt_handle& stmt) -> stdx::option<const_value>;
    auto eval_decl(ast::node_id id, const ast::decl_stmt& decl) -> stdx::option<const_value>;
    auto eval_block(ast::node_id id, const ast::block_stmt& block) -> stdx::option<const_value>;
    auto eval_label(ast::node_id id, const ast::label_expr& label) -> stdx::option<const_value>;
    auto eval_if(ast::node_id id, const ast::if_expr& if_expr) -> stdx::option<const_value>;
    // Counts one iteration, flagging unknown control flow once the unroll limit is reached
    auto exceeded_unroll_limit(usize& iterations) -> bool;
    // A loop condition folded to `bool`; `none` (and unknown control flow) otherwise
    auto eval_loop_condition(ast::expr_handle condition) -> stdx::option<bool>;
    // Consumes an unlabeled `break` / `continue` aimed at the current loop
    auto consume_loop_signal(stdx::option<std::string_view> own_label) -> loop_step;
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
    // With `base`, the slice is a view of that compile-time place rather than a copy
    auto eval_slice_index(ast::node_id            id,
                          const const_value&      target_val,
                          ast::node_id            range_id,
                          const ast::range_expr&  range,
                          stdx::option<const_ref> base = stdx::none) -> stdx::option<const_value>;
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
    [[nodiscard]] auto eval_flow(ast::expr_handle operand, ast::node_id at)
        -> stdx::option<flow_eval>;
    // Binds an `if`/`while` capture from a folded `Flow` in the current frame
    [[nodiscard]] auto
    bind_flow_capture(const ast::capture& capture, const flow_eval& flow, ast::node_id at) -> bool;
    auto match_pattern(const ast::match_pattern_handle& pattern_h, const const_value& target)
        -> bool;

    auto lookup_local_binding(std::string_view name) const noexcept -> stdx::option<const_value>;
    auto set_local_binding(std::string_view name, const_value val) -> bool;
    auto write_target(ast::node_id target, const_value val) -> bool;

    // Simulates execution of a statement preceding the current evaluation site.
    // Dispatches declarations, expressions/assignments, sub-blocks, and control-flow signals.
    auto simulate_stmt(const ast::stmt_handle& stmt) -> void;
    auto simulate_expr(ast::node_id id) -> void;

    // Bails if the decl does not have a comptime modifier
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
    auto simulate_label(const ast::label_expr& label) -> void;

  private:
    // Names of mutable `comptime let mut` bindings currently in scope during simulation.
    ankerl::unordered_dense::set<std::string_view> active_cx_vars_;
    usize                                          max_recursion_depth_{256};
    std::vector<usize>                             recursion_limit_stack_;
    sema::context&                                 ctx_;
    gsl::not_null<mod::module*>                    module_;
    stdx::option<mod::module&>                     vtable_root_;
    stdx::option<sema::type&>                      enclosing_type_;
    stdx::option<const symbol_scoping&>            symbol_scoping_;
    std::vector<call_frame>                        call_stack_;
    u64                                            next_frame_id_{1};
    // Stable names for the hidden slots `ref_to_temporary` binds
    std::deque<std::string> temporary_names_;
    // The return type of each compile-time call being evaluated, innermost last
    std::vector<stdx::option<sema::type&>> return_types_;
    default_counter                        recursion_depth_;

    // A label's name, handed to the loop it directly wraps so it can consume jumps aimed at it
    stdx::option<std::string_view> pending_loop_label_;
    bool                           cond_unknown_{false};
    bool                           comptime_context_{false};
    eval_signal                    current_signal_{};
    stdx::option<const_value>      current_error_val_{};

    ankerl::unordered_dense::map<memo_key, const_value, memo_key_hash> memo_cache_;
    ankerl::unordered_dense::map<std::string, const_value>             global_cx_vars_;
};

} // namespace ghoti::gir
