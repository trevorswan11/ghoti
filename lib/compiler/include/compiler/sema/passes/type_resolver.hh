#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"
#include "support/scope_guard.hh"

namespace ghoti::sema {

// Resolves all types and symbol uses without type checking
class type_resolver {
  public:
    struct structural_validator {
        std::vector<std::string_view>                  provided;
        std::vector<std::string_view>                  duplicates;
        std::vector<std::string_view>                  missings;
        std::vector<std::string_view>                  unknowns;
        ankerl::unordered_dense::set<std::string_view> seen;

        // State is cleared but not reallocated to save some time
        auto clear() noexcept -> void {
            provided.clear();
            duplicates.clear();
            missings.clear();
            unknowns.clear();
            seen.clear();
        }
    };

  public:
    static auto resolve_types(mod::module& module, context& ctx) -> mod::module_state;

    template <ast::IndexableID ID> auto resolve(ID id) -> void {
        resolving_.ast[id].visit([&](const auto& data) -> void { visit(id, data); });
    }

    // Resolves `I` / `T` for every non-parameterized `impl` root and records a skeleton in the
    // impl registry, so `@implements` and method resolution see impls regardless of source order.
    auto pre_register_impls() -> void;
    // Records a parameterized `impl(P) [I for] Ctor(P)` root, keyed on its base ctor, for later
    // per-monomorphization expansion.
    auto register_parameterized_impl(ast::node_id root, const ast::impl_stmt& impl) -> void;
    // Resolves a parameterized impl body once against opaque sentinels + dummy `constexpr`
    // values and stores the result on the shared `impl_registry`
    auto build_param_impl_template(const ast::impl_stmt& impl, ast::node_id site) -> void;
    // Folds each parameter-dependent `if constexpr` / `match constexpr` recorded in `tmpl` with
    // this instantiation's real bindings, appending the verdicts to `typing`.
    auto
    refold_param_impl_branches(mod::module&                                              impl_mod,
                               const param_impl_template&                                tmpl,
                               body_type_diff&                                           typing,
                               gsl::span<const std::pair<std::string, gir::const_value>> bindings)
        -> void;
    auto resolve_impl_type_ref(ast::explicit_type_id ref) -> type&;
    auto resolve_impl_method_access(const type& target, std::string_view name, source_location loc)
        -> stdx::option<stdx::result<gsl::not_null<type*>, diagnostic>>;
    // Resolves and records the interface bounds of a generic function's `impl I` parameters.
    auto register_impl_param_bounds(type& fn_type, const ast::function_expr& fn) -> void;

    // Expands every parameterized `impl(P) ...` whose base ctor is `base_ctor_fn` for the freshly
    // materialized concrete target `concrete`, remapping its template typing and recording one
    // `impl_record` + emit entries per method
    auto instantiate_impls_for(type&                  concrete,
                               const mod::module&     base_mod,
                               ast::node_id           base_ctor_fn,
                               gsl::span<type* const> ctor_args,
                               gsl::span<const std::pair<std::string, gir::const_value>> ctor_cx,
                               const ast::function_expr&                                 base_fn,
                               std::string_view ctor_mangled) -> void;

  private:
    using scope = symbol_table_stack::scope;
    using named_test_map_t =
        ankerl::unordered_dense::map<std::string_view, ast::handle<ast::node_kind::TEST_STATEMENT>>;
    using function_boundary_guard = scope_guard<std::vector<usize>>;
    using open_function_guard     = scope_guard<std::vector<ast::node_id>>;
    using self_recursion_guard    = scope_guard<std::vector<bool>>;

    // Sets the flag to the provided value and resets it on destruction
    class mutating_context_guard {
      public:
        explicit mutating_context_guard(bool& flag, bool value = true) noexcept
            : flag_{flag}, prev_{flag} {
            flag_ = value;
        }
        ~mutating_context_guard() noexcept { flag_ = prev_; }
        MAKE_PINNED(mutating_context_guard);

      private:
        bool& flag_;
        bool  prev_;
    };

    // Tracks collected return types within function bodies for auto return type inference
    struct return_tracker {
        std::vector<gsl::not_null<type*>> return_types{};
        bool                              is_auto_return{false};
        stdx::option<type&>               expected_type;

        auto               add_return(type& t) -> void { return_types.emplace_back(&t); }
        [[nodiscard]] auto has_returns() const noexcept -> bool { return !return_types.empty(); }
        [[nodiscard]] auto deduced_return_type(context& ctx) const noexcept -> type& {
            if (return_types.empty()) { return ctx.get_builtin_resolved_type(type_kind::VOID_); }
            return *return_types.front();
        }
    };

    // A flag for helper functions to indicate if their resolution was poisoned
    enum class resolve_result : u8 {
        OK,
        POISONED,
    };

    // A guarded stack that manages the current user-types for function self parameters
    class structural_type_stack {
      public:
        using guard = scope_guard<structural_type_stack>;

      public:
        structural_type_stack() noexcept = default;
        ~structural_type_stack()         = default;
        MAKE_MOVE_CONSTRUCTABLE_ONLY(structural_type_stack);

        auto push(type& type) -> void { stack_.emplace_back(&type); }
        auto pop() noexcept -> void {
            if (!stack_.empty()) { stack_.pop_back(); }
        }

        // Only returns none when there are no types in the stack
        [[nodiscard]] auto peek() const noexcept -> stdx::option<type&> {
            if (stack_.empty()) { return stdx::none; }
            return *stack_.back();
        }

      private:
        std::vector<gsl::not_null<type*>> stack_;
    };

    using structural_guard      = structural_type_stack::guard;
    using constexpr_frame_guard = scope_guard<std::vector<constexpr_frame>>;

    // Resolves the provided type and unresolves upon destruction if not committed
    template <typename Resolvee> class committable_resolution {
      public:
        template <typename... Args>
        explicit committable_resolution(type& type, Args&&... resolvee) : type_{type} {
            type_.resolve<Resolvee>(std::forward<Args>(resolvee)...);
        }

        ~committable_resolution() {
            if (!committed_) { type_.unresolve(); }
        }

        // This action cannot be undone, defer until the very end!
        auto commit() noexcept -> void { committed_ = true; }

      private:
        type& type_;
        bool  committed_{false};
    };

    // `impl I` / `impl (A + B)` parameter bounds, mapping generic fn type to the interface types it
    // must implement.
    using impl_param_bound_map_t =
        ankerl::unordered_dense::map<const type*,
                                     std::vector<std::pair<u32, std::vector<const type*>>>>;

  private:
    auto visit(ast::node_id, const ast::array_expr&) -> void;
    auto visit(ast::node_id, const ast::asm_expr&) -> void;

    // This is meant to be called after the arguments have all been resolved
    template <ast::IndexableID ID>
    [[nodiscard]] auto
    resolve_builtin_call(ID id, const ast::call_expr& call, const types::builtin_function& builtin)
        -> stdx::result<void, diagnostic>;

    auto resolve_call_args(gsl::span<const ast::call_expr::argument> args) -> resolve_result;
    [[nodiscard]] auto get_resolved_call_arg_type(const ast::call_expr::argument& arg)
        -> gsl::not_null<type*>;
    [[nodiscard]] auto get_call_arg_location(const ast::call_expr::argument& arg)
        -> source_location;

    // Whether the outermost callee of `call` resolves to a `@discardable` declaration
    [[nodiscard]] auto callee_is_discardable(const ast::call_expr& call) const -> bool;
    // Errors on a non-`void` call result dropped in statement position
    auto check_unused_result(ast::node_id stmt_id, const ast::expr_stmt& stmt) -> void;

    [[nodiscard]] auto local_const_fn_ref(ast::expr_handle expr) -> stdx::option<gir::const_value>;
    [[nodiscard]] auto constexpr_closure_value(ast::expr_handle expr)
        -> stdx::option<gir::const_value>;

    template <ast::IndexableID ID> auto resolve_call(ID, const ast::call_expr&) -> void;
    auto                                visit(ast::node_id, const ast::call_expr&) -> void;
    auto                                visit(ast::node_id, const ast::do_while_loop_expr&) -> void;

    // Returns the same type buffer that it was passed when resolution was successful
    [[nodiscard]] auto resolve_members(gsl::span<type*>                    buf,
                                       gsl::span<const ast::member_handle> members)
        -> stdx::option<gsl::span<type*>>;
    template <ast::IndexableID ID> auto visit(ID, const ast::enum_expr&) -> void;

    auto visit(ast::node_id, const ast::for_loop_expr&) -> void;
    auto visit(ast::node_id, const ast::function_expr&) -> void;

    template <ast::IndexableID ID> auto resolve_symbol(ID, symbol&) -> void;
    template <ast::IndexableID ID> auto resolve_ident(ID, const ast::identifier_expr&) -> void;

    // Records, for the reference node `ref_id`, the symbol-table index `owner_table_idx` that
    // owns `sym`, iff `sym` names a top-level function or `var` global
    auto record_symbol_owner(ast::node_id       ref_id,
                             usize              owner_table_idx,
                             const mod::module& target_mod,
                             const symbol&      sym) -> void;

    // Unwraps pointer/reference wrappers, then resolves `member` in the aggregate's
    // own symbol table.
    auto record_member_owner(ast::node_id ref_id, type& object_type, ast::identifier_handle member)
        -> void;

    // If `call` invokes a member function of a `fn(...): type` constructor result, pins the call
    // to that instantiation's mangled member symbol so the emitter targets the right monomorph.
    auto record_type_ctor_member_call(ast::node_id call_id, const ast::call_expr& call) -> void;
    auto register_non_generic_type_ctor_members(type& result, const ast::call_expr& ctor_call)
        -> void;

    [[nodiscard]] auto get_resolved_symbol_type(symbol::data_t& symbol_data) -> type&;

    // Infers how a closure should store one free variable, given how it's used in the body.
    // `force_move` overrides the inferred mode to VALUE unconditionally (`move fn`)
    [[nodiscard]] auto infer_capture_mode(capture_usage usage,
                                          const type&   captured_type,
                                          bool          force_move) noexcept -> types::capture_mode;

    // Builds and attaches a `types::closure_t` for a function_expr with a non-empty capture list
    auto attach_closure_type(ast::node_id fn_id, type& fn_type, bool is_move) -> type&;

    auto visit(ast::node_id, const ast::identifier_expr&) -> void;
    auto visit(ast::node_id, const ast::if_expr&) -> void;
    auto visit(ast::node_id, const ast::index_expr&) -> void;
    auto visit(ast::node_id, const ast::infinite_loop_expr&) -> void;
    auto visit(ast::node_id, const ast::cfg_value_expr&) -> void;
    auto visit(ast::node_id, const ast::assignment_expr&) -> void;
    auto visit(ast::node_id, const ast::binary_expr&) -> void;

    // Attempts to access the given member in the provided structural type
    [[nodiscard]] auto
    resolve_structural_access(type&                          object_type,
                              ast::identifier_handle         member,
                              source_location                object_location,
                              stdx::option<std::string_view> object_name = stdx::none)
        -> stdx::result<gsl::not_null<type*>, diagnostic>;

    // Retrieve's the rightmost identifier name from the accessor
    [[nodiscard]] auto get_rightmost_name(ast::expr_handle) const noexcept
        -> stdx::option<std::string_view>;
    template <ast::IndexableID ID> auto resolve_dot(ID, const ast::dot_expr&) -> void;

    auto visit(ast::node_id, const ast::dot_expr&) -> void;
    auto visit(ast::node_id, const ast::range_expr&) -> void;

    [[nodiscard]] auto validate_struct_initializer(ast::node_id,
                                                   const ast::initializer_expr&,
                                                   type&) -> stdx::result<void, diagnostic>;

    auto visit(ast::node_id, const ast::initializer_expr&) -> void;
    auto visit(ast::node_id, const ast::label_expr&) -> void;

    [[nodiscard]] auto validate_enum_arms(ast::node_id, const ast::match_expr&, type&)
        -> stdx::option<diagnostic>;

    [[nodiscard]] auto validate_union_arms(ast::node_id, const ast::match_expr&, type&)
        -> stdx::option<diagnostic>;

    // Resolves a `match` whose scrutinee is a compile-time `type` value
    auto resolve_type_match(ast::node_id, const ast::match_expr&, type& matcher_type) -> void;

    // Resolves a `match constexpr`: folds the scrutinee, type-checks only the selected arm
    auto resolve_constexpr_match(ast::node_id, const ast::match_expr&, type& matcher_type) -> void;
    // Template-mode: type-resolve every arm of a `match constexpr` without folding, so
    // `instantiate_impls_for` can pick the arm per instantiation.
    auto resolve_constexpr_match_all_arms(ast::node_id, const ast::match_expr&, type& matcher_type)
        -> void;

    auto visit(ast::node_id, const ast::match_expr&) -> void;
    auto visit(ast::node_id, const ast::reference_expr&) -> void;
    auto visit(ast::node_id, const ast::address_of_expr&) -> void;
    auto visit(ast::node_id, const ast::dereference_expr&) -> void;
    auto visit(ast::node_id, const ast::unary_expr&) -> void;
    auto visit(ast::node_id, const ast::unwrap_expr&) -> void;
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

    template <ast::IndexableID ID>
    auto resolve_module_access(ID, const ast::module_access_expr&) -> void;
    auto visit(ast::node_id, const ast::module_access_expr&) -> void;

    template <ast::IndexableID ID> auto visit(ID, const ast::struct_expr&) -> void;
    template <ast::IndexableID ID> auto visit(ID, const ast::union_expr&) -> void;
    template <ast::IndexableID ID> auto visit(ID, const ast::interface_expr&) -> void;
    auto resolve_required_method_type(const ast::function_expr& fn, type& self_placeholder)
        -> type&;
    auto visit(ast::node_id, const ast::while_loop_expr&) -> void;

    auto visit(ast::node_id, const ast::block_stmt&) -> void;

    // Returns `true` if the resolution was successful
    [[nodiscard]] auto resolve_control_flow_label(stdx::option<ast::identifier_handle> label,
                                                  std::string_view                     stmt_name)
        -> stdx::result<stdx::option<symbol&>, diagnostic>;

    auto visit(ast::node_id, const ast::break_stmt&) -> void;
    auto visit(ast::node_id, const ast::cfg_stmt&) -> void;
    auto visit(ast::node_id, const ast::continue_stmt&) -> void;
    auto visit(ast::node_id, const ast::decl_stmt&) -> void;
    auto visit(ast::node_id, const ast::defer_stmt&) -> void;
    auto visit(ast::node_id, const ast::discard_stmt&) -> void;
    auto visit(ast::node_id, const ast::expr_stmt&) -> void;
    auto visit(ast::node_id, const ast::impl_stmt&) -> void;
    // Checks required methods present & signature-compatible, required associated items bound, no
    // stray members.
    auto check_impl_conformance(const impl_record& rec, const types::interface_t& iface) -> void;
    auto visit(ast::node_id, const ast::import_stmt&) -> void;
    auto visit(ast::node_id, const ast::return_stmt&) -> void;
    auto visit(ast::node_id, const ast::test_stmt&) -> void;
    auto visit(ast::node_id, const ast::using_stmt&) -> void;
    auto visit(ast::node_id, ast::discarded) noexcept -> void {}

    // Creates a potentially new type with the id-stored modifiers
    auto apply_explicit_modifiers(ast::explicit_type_id id, type& inner_type) -> type&;

    auto visit(ast::explicit_type_id, const ast::identifier_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::module_access_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::dot_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::call_expr&) -> void;
    auto visit(ast::explicit_type_id, const ast::explicit_function_type&) -> void;
    auto visit(ast::explicit_type_id, ast::explicit_type_id) -> void;
    auto visit(ast::explicit_type_id, const ast::explicit_array_type&) -> void;

    // Looks up the symbol by name in the current index ONLY. Changes no state on failure
    auto resolve_symbol_info(ast::identifier_handle handle, stdx::option<symbol_kind> kind)
        -> stdx::option<symbol&>;

    auto instantiate_generic(type&                             callee_type,
                             const generic_function_info&      fn_info,
                             gsl::span<type*>                  concrete_args,
                             gsl::span<const gir::const_value> constexpr_args = {})
        -> stdx::option<generic_instantiation_entry>;

    type_resolver(mod::module& resolving, context& ctx)
        : resolving_{resolving}, table_idx_{*resolving.root_table_idx}, ctx_{ctx} {
        VERIFY(ctx.prelude_index, "TypeResolver must be used after prelude-injection");
        table_stack_.push(*ctx_.prelude_index);
        table_stack_.push(table_idx_);
    }

    type_resolver(mod::module&       resolving,
                  context&           ctx,
                  usize              table_idx,
                  symbol_table_stack table_stack)
        : resolving_{resolving}, table_idx_{table_idx}, table_stack_{std::move(table_stack)},
          ctx_{ctx} {
        VERIFY(ctx.prelude_index, "TypeResolver must be used after prelude-injection");
    }

  private:
    mod::module&              resolving_;
    usize                     table_idx_;
    symbol_table_stack        table_stack_;
    structural_type_stack     user_type_stack_;
    structural_type_stack     implicit_type_stack_;
    std::vector<usize>        function_boundaries_;
    std::vector<ast::node_id> open_function_nodes_;
    std::vector<bool>         self_recursive_flags_;

    bool in_mutating_context_{false};
    bool for_generic_instantiation_{false};
    bool in_subscript_index_{false};
    bool in_for_iterable_{false};
    // Set only while `build_param_impl_template` resolves a parameterized-impl body against
    // sentinel / dummy parameter values
    bool                      building_param_template_{false};
    std::vector<ast::node_id> template_constexpr_ifs_;
    std::vector<ast::node_id> template_constexpr_matches_;
    stdx::opt_size            pending_impl_method_owner_;
    stdx::option<std::string> pending_param_impl_target_;

    // Distinct opaque `type` sentinel used to stand in for one parameterized-impl type param
    // during its template resolution, keyed on the param's name-node index. The template itself
    // and the per-target expansion set live on the shared `impl_registry` so a monomorphization
    // triggered from any consuming module can find them.
    auto param_impl_sentinel(usize disc) -> type&;

    impl_param_bound_map_t impl_param_bounds_;
    named_test_map_t       named_tests_;
    structural_validator   struct_validator_;
    structural_validator   enum_validator_;
    structural_validator   union_validator_;

    context&                    ctx_;
    stdx::option<type&>         last_type_;
    std::vector<return_tracker> return_trackers_;
};

} // namespace ghoti::sema
