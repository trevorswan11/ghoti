#pragma once

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/builder.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/symbol_scoping.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"
#include "support/int128.hh"
#include "support/scope_guard.hh"

namespace ghoti::gir {

class emitter {
  public:
    explicit emitter(sema::context& ctx, mod::module& ast_mod) noexcept
        : ctx_{ctx}, ast_module_{ast_mod}, const_eval_{ctx_, ast_mod},
          gir_module_{ast_mod, ctx_.arena}, runtime_safety_{ctx.runtime_safety},
          target_ptr_bits_{codegen::target_facts::resolve(ctx.target_opts.triple_str).ptr_bits} {}
    ~emitter() = default;
    MAKE_PINNED(emitter);

    [[nodiscard]] auto emit(bool include_builtin_test_runtime = false) -> module;

  private:
    struct local_binding {
        local_id            id;
        sema::type&         type;
        bool                is_alloca{false};
        stdx::option<value> const_val;
        bool                is_const{false};
    };

    struct loop_context {
        stdx::option<std::string_view> label;
        segment_id                     break_target{0};
        segment_id                     continue_target{0};
        stdx::option<local_id>         result_slot;
    };

    struct iterable_info {
        bool                           is_range{false};
        bool                           is_inclusive{false};
        bool                           range_open_upper{false};
        local_id                       var_slot;
        stdx::option<sema::type&>      elem_type;
        value                          end_val{};
        stdx::option<std::string_view> capture_name;
        stdx::option<value>            arr_val{};
        stdx::option<sema::type&>      capture_type;
        bool alias_capture{false}; // the capture was written `|&x|`/`|^x|`
    };

    struct scope_frame {
        ankerl::unordered_dense::map<std::string_view, local_binding> bindings;
        std::vector<ast::stmt_handle>                                 defers;
    };

    using scope_guard           = ghoti::scope_guard<std::vector<scope_frame>>;
    using loop_context_guard    = ghoti::scope_guard<std::vector<loop_context>>;
    using open_fn_name_guard    = ghoti::scope_guard<std::vector<std::string>>;
    using open_fn_closure_guard = ghoti::scope_guard<std::vector<bool>>;
    using constexpr_frame_guard = ghoti::scope_guard<std::vector<sema::constexpr_frame>>;
    using type_guard            = ghoti::scope_guard<std::vector<sema::type*>>;

  private:
    auto emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto emit_top_level_using(ast::node_id id, const ast::using_stmt& using_stmt) -> void;
    auto emit_top_level_test(ast::node_id id, const ast::test_stmt& test) -> void;
    // Emits the member functions of an `impl [I for] T { ... }` block under names scoped to the
    // impl's own symbol table, plus any interface default methods the impl inherits.
    auto emit_top_level_impl(ast::node_id id, const ast::impl_stmt& impl) -> void;
    // Emits one inherited interface default-method body for a concrete impl target. The body's
    // AST lives in `iface_mod`; `self` is retyped to the target, bare `self.method(...)` calls
    // are redirected to the impl's own methods, and `body_type_diff[typing_key]` is replayed so
    // associated types resolve to the impl's bindings.
    auto emit_impl_default_method(std::string_view          gir_name,
                                  usize                     impl_scope_idx,
                                  mod::module&              iface_mod,
                                  ast::node_id              sig_id,
                                  const ast::function_expr& fn_expr,
                                  stdx::option<sema::type&> concrete_sig,
                                  std::string_view          typing_key) -> void;

    auto emit_function(ast::node_id                   id,
                       const ast::decl_stmt&          decl,
                       const ast::function_expr&      fn_expr,
                       stdx::option<std::string_view> name_override = stdx::none) -> void;

    // Emits one member function of a `fn(...): type` constructor's result as a method of its
    // per-instantiation aggregate type. Registered by the resolver in `type_ctor_member_emits`.
    auto emit_type_ctor_member(mod::module& owner_mod, const sema::type_ctor_member_emit& tcm)
        -> void;
    auto emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr) -> std::string;
    // Like emit_anonymous_function, but pre-binds `name` to itself so the body can self-recurse
    auto emit_named_local_function(std::string_view          name,
                                   ast::node_id              id,
                                   const ast::function_expr& fn_expr) -> std::string;

    // Emits a capturing function_expr's implementation (once, idempotently) and constructs its
    // environment value at the current (definition-site) insertion point
    auto emit_closure(ast::node_id id, const ast::function_expr& fn_expr) -> value;
    auto emit_closure_function(const ast::function_expr&     fn_expr,
                               const sema::types::closure_t& cl,
                               sema::type&                   closure_type) -> void;
    auto emit_closure_env(const sema::types::closure_t& cl, sema::type& closure_type) -> value;

    // Emits as a plain non-capturing fn with its captures baked in as constants
    auto emit_constexpr_closure(const const_closure& cl) -> std::string;

    // Reads a captured variable's current value (VALUE mode) or address (REF/MUT_REF mode) from
    // the definition-site scope, for use when constructing an environment field
    auto get_capture_source(const sema::types::closure_capture& capture) -> value;

    auto               emit_stmt(const ast::stmt_handle& stmt) -> void;
    auto               emit_block(const ast::block_stmt& block) -> void;
    auto               emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto               emit_return_stmt(ast::node_id id, const ast::return_stmt& ret) -> void;
    auto               emit_defer_stmt(ast::node_id id, const ast::defer_stmt& def) -> void;
    auto               emit_break(ast::node_id id, const ast::break_stmt& brk) -> void;
    auto               emit_continue(ast::node_id id, const ast::continue_stmt& cnt) -> void;
    [[nodiscard]] auto emit_stmt_as_value(const ast::stmt_handle& stmt) -> value;
    [[nodiscard]] auto retype_if_undefined(value v, sema::type& result_type) -> value;

    auto emit_defers_for_scope(usize scope_idx) -> void;
    auto emit_defers_up_to(usize target_depth) -> void;
    auto emit_lvalue(ast::node_id id) -> value;

    // Emits a `panic_handler(msg, file, line, column)` call followed by `unreachable`
    auto emit_panic_call(std::string_view message, ast::node_id site) -> void;
    auto emit_enum_cast_guard(ast::node_id     site,
                              const value&     enum_val,
                              const value&     src_val,
                              ast::expr_handle src_expr) -> void;

    [[nodiscard]] auto enum_discriminants(const sema::types::enum_t& en) -> std::vector<i64>;
    auto               emit_runtime_tag_name(ast::expr_handle operand_expr,
                                             sema::type&      operand_type,
                                             sema::type&      ret_type) -> stdx::option<value>;

    // Builds a genuine `{ptr, len}` slice value naming `text`, safe to store anywhere
    auto materialize_string_slice(std::string_view text, sema::type& slice_type) -> value;

    auto emit_null_pointer_check(value ptr, ast::node_id site) -> void;
    // `wrapping` forces the overflow guard off unconditionally, for `+% -% *% <<%` / `-%x`
    auto emit_checked_binary(instruction_kind kind,
                             value            lhs,
                             value            rhs,
                             sema::type&      result_type,
                             ast::node_id     site,
                             bool             wrapping = false) -> local_id;
    auto emit_checked_unary(instruction_kind kind,
                            value            operand,
                            sema::type&      result_type,
                            ast::node_id     site,
                            bool             wrapping = false) -> local_id;

    auto pointer_to_bool(value ptr, bool invert) -> value;
    auto coerce_condition(value cond) -> value;
    auto request_builtin_runtime(std::string_view name) -> void;
    auto ensure_builtin_runtime(std::string_view name) -> void;
    auto spill_to_temporary(value val, sema::type& type, bool is_const = false) -> value;
    auto lvalue_of_expr(ast::node_id id, sema::type& sema_type) -> value;

    // An escape hatch for materializing constant evaluated aggregates since they cannot
    // otherwise be represented as GIR instructions
    auto materialize_const(const const_value& cv) -> value;

    auto emit_expression(const ast::expr_handle& expr) -> value {
        return emit_expression_id(*expr);
    }
    auto emit_array(ast::node_id id, const ast::array_expr& array) -> value;
    auto emit_slice_from_array(value arr_lval, const sema::type& arr_type) -> value;
    // Builds a `&dyn I` / `^dyn I` fat pointer `{ data, vtable }` from `src` (a `&T` / `^T`)
    auto emit_dyn_coercion(ast::expr_handle src, const sema::type& fat_type) -> value;
    // Lowers `expr[lo..{=}hi]` on an array or slice to a bounds-checked `{ptr, len}` subslice.
    auto emit_slice_range(ast::node_id id, const ast::index_expr& index) -> value;
    auto emit_coerced_expr(ast::expr_handle expr_id, const sema::type& dest_type) -> value;
    auto emit_generic_instantiation(const sema::generic_instantiation_request& req) -> void;
    auto emit_expression_id(ast::node_id id) -> value;
    // Produces a value exactly as resolved, including a bare reference-typed value where
    // applicable.
    auto emit_expression_id_raw(ast::node_id id) -> value;
    auto emit_if(ast::node_id id, const ast::if_expr& if_expr) -> value;
    auto emit_match(ast::node_id id, const ast::match_expr& match) -> value;

    // @mem* decompose the slice args into a data pointer + byte length
    auto emit_mem_intrinsic(ast::node_id          id,
                            const ast::call_expr& call,
                            syntax::token_type_t  builtin) -> void;

    auto emit_unwrap(ast::node_id id, const ast::unwrap_expr& unwrap) -> value;
    auto emit_unwrap_propagation(value             operand_addr,
                                 const sema::type& operand_union,
                                 u64               operand_diverge_idx,
                                 bool              diverge_is_void,
                                 ast::node_id      site) -> void;
    auto emit_union_active_field_guard(value            union_addr,
                                       u64              field_idx,
                                       std::string_view field_name,
                                       ast::node_id     site) -> void;
    auto emit_initializer(ast::node_id id, const ast::initializer_expr& init) -> value;
    // Emits a struct field's `= default` expression, coerced to `field_type`
    auto               emit_field_default(ast::expr_handle   default_expr,
                                          const mod::module& owner,
                                          const sema::type&  field_type) -> value;
    auto               emit_dot(ast::node_id id, const ast::dot_expr& dot) -> value;
    [[nodiscard]] auto dot_object_is_type_namespace(const ast::dot_expr& dot) -> bool;
    auto               emit_index(ast::node_id id, const ast::index_expr& index) -> value;
    auto               emit_address_of(ast::node_id id, const ast::address_of_expr& addr) -> value;
    auto emit_dereference(ast::node_id id, const ast::dereference_expr& deref) -> value;
    auto emit_reference(ast::node_id id, const ast::reference_expr& ref) -> value;
    auto emit_implicit_access(ast::node_id id, const ast::implicit_access_expr& imp) -> value;
    auto emit_module_access(ast::node_id id, const ast::module_access_expr& mod_access) -> value;
    auto emit_while(ast::node_id                   id,
                    const ast::while_loop_expr&    while_loop,
                    stdx::option<std::string_view> label       = stdx::none,
                    stdx::option<local_id>         res_slot    = stdx::none,
                    stdx::option<sema::type&>      result_type = stdx::none) -> value;
    auto emit_do_while(ast::node_id                   id,
                       const ast::do_while_loop_expr& do_while,
                       stdx::option<std::string_view> label       = stdx::none,
                       stdx::option<local_id>         res_slot    = stdx::none,
                       stdx::option<sema::type&>      result_type = stdx::none) -> value;
    auto emit_infinite_loop(ast::node_id                   id,
                            const ast::infinite_loop_expr& loop,
                            stdx::option<std::string_view> label       = stdx::none,
                            stdx::option<local_id>         res_slot    = stdx::none,
                            stdx::option<sema::type&>      result_type = stdx::none) -> value;
    auto emit_for(ast::node_id                   id,
                  const ast::for_loop_expr&      for_loop,
                  stdx::option<std::string_view> label       = stdx::none,
                  stdx::option<local_id>         res_slot    = stdx::none,
                  stdx::option<sema::type&>      result_type = stdx::none) -> value;
    auto emit_label(ast::node_id id, const ast::label_expr& label) -> value;
    auto emit_binary(ast::node_id id, const ast::binary_expr& binary) -> value;
    // Detects `union_val == .field` and emits a tag comparison instead of a union-vs-field-type EQ
    auto try_emit_union_field_eq(ast::node_id lhs, ast::node_id rhs) -> stdx::option<local_id>;
    auto emit_union_tag_eq(value union_addr, ast::node_id member_pattern_id) -> local_id;
    auto emit_logical_and(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_logical_or(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value;
    // Keeps a tagged union's runtime discriminant in sync with a direct `union.field = ...` write
    auto sync_tagged_union_tag(ast::node_id assign_lhs) -> void;
    auto emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value;

    // Bit-packed `packed struct`/`packed union` field access: shift/mask over the backing int.
    [[nodiscard]] auto emit_packed_field_read(value                        backing_addr,
                                              const sema::types::struct_t& st,
                                              usize                        field_idx,
                                              sema::type&                  field_type) -> value;
    [[nodiscard]] auto emit_packed_field_extract(value                        backing,
                                                 const sema::types::struct_t& st,
                                                 usize                        field_idx,
                                                 sema::type&                  field_type) -> value;
    auto               emit_packed_field_write(value                        backing_addr,
                                               const sema::types::struct_t& st,
                                               usize                        field_idx,
                                               sema::type&                  field_type,
                                               value                        new_field_val) -> void;
    [[nodiscard]] auto emit_packed_field_read(value                       backing_addr,
                                              const sema::types::union_t& ut,
                                              sema::type&                 field_type) -> value;
    auto               emit_packed_field_write(value                       backing_addr,
                                               const sema::types::union_t& ut,
                                               sema::type&                 field_type,
                                               value                       new_field_val) -> void;
    [[nodiscard]] auto
    emit_packed_bits_extract(value backing, u32 n, u32 offset, u32 fbits, sema::type& field_type)
        -> value;
    auto               emit_packed_bits_insert(value       backing_addr,
                                               u32         n,
                                               u32         offset,
                                               u32         fbits,
                                               sema::type& field_type,
                                               value       new_field_val) -> void;
    [[nodiscard]] auto emit_packed_bits_merge(value       old_backing,
                                              u32         n,
                                              u32         offset,
                                              u32         fbits,
                                              sema::type& field_type,
                                              value       new_field_val) -> value;

    // The bit layout of one field within its bit-packed enclosing aggregate.
    struct packed_field_layout {
        u32         n{1};      // backing-integer width of the enclosing aggregate
        u32         offset{0}; // LSB-first bit offset of the field (always 0 for a union)
        u32         fbits{1};  // width of the field
        usize       field_idx{0};
        sema::type* field_type{nullptr};
    };
    [[nodiscard]] auto packed_layout_of(const ast::dot_expr& dot) -> packed_field_layout;

    // The i64/u64/i128/u128 payload of a compile-time integer `value`, as a 128-bit signed
    // int; none when `v` has no integer payload.
    [[nodiscard]] static auto folded_int(const value& v) noexcept -> stdx::option<i128>;
    // Emits a `LITERAL_OUT_OF_RANGE` diagnostic at `at` if the `constexpr_int` `v` does not
    // fit `target` (a concrete integer type). Returns `v` retyped to `target`.
    [[nodiscard]] auto coerce_constexpr_int(value v, sema::type& target, ast::node_id at) -> value;
    auto               emit_packed_store(const ast::dot_expr& dot, value field_val) -> void;
    [[nodiscard]] auto emit_packed_field_assign(ast::node_id                id,
                                                const ast::dot_expr&        dot,
                                                const ast::assignment_expr& assign,
                                                syntax::token_type_t        op_type) -> value;
    auto               emit_call(ast::node_id id, const ast::call_expr& call) -> value;
    auto               emit_asm(ast::node_id id, const ast::asm_expr& node) -> value;
    auto               emit_ident(ast::node_id id, const ast::identifier_expr& ident) -> value;
    // Lvalue (address) of a `var`-style global backed by a GIR global.
    [[nodiscard]] auto global_ref_in(usize table_idx, std::string_view name) -> stdx::option<value>;
    [[nodiscard]] auto try_global_ref(std::string_view name) -> stdx::option<value>;
    [[nodiscard]] auto try_static_member_ref(const sema::type& owner, std::string_view member)
        -> stdx::option<value>;

    template <stdx::Reference Binding = const local_binding&>
    auto lookup_binding(std::string_view name) noexcept -> stdx::option<Binding> {
        PROFILE_FUNCTION();
        for (auto& frame : scopes_ | std::views::reverse) {
            if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) {
                return stdx::option<Binding>{it->second};
            }
        }
        return stdx::none;
    }

    [[nodiscard]] static constexpr auto map_binary_op(syntax::token_type_t tok) noexcept
        -> stdx::option<instruction_kind> {
        switch (tok) {
        case syntax::token_type_t::PLUS:          return instruction_kind::ADD;
        case syntax::token_type_t::PLUS_PERCENT:  return instruction_kind::ADD;
        case syntax::token_type_t::MINUS:         return instruction_kind::SUB;
        case syntax::token_type_t::MINUS_PERCENT: return instruction_kind::SUB;
        case syntax::token_type_t::STAR:          return instruction_kind::MUL;
        case syntax::token_type_t::STAR_PERCENT:  return instruction_kind::MUL;
        case syntax::token_type_t::SLASH:         return instruction_kind::DIV;
        case syntax::token_type_t::PERCENT:       return instruction_kind::MOD;
        case syntax::token_type_t::BW_AND:        return instruction_kind::AND;
        case syntax::token_type_t::BW_OR:         return instruction_kind::OR;
        case syntax::token_type_t::CARET:         return instruction_kind::XOR;
        case syntax::token_type_t::SHL:           return instruction_kind::SHL;
        case syntax::token_type_t::SHL_PERCENT:   return instruction_kind::SHL;
        case syntax::token_type_t::SHR:           return instruction_kind::SHR;
        case syntax::token_type_t::EQ:            return instruction_kind::EQ;
        case syntax::token_type_t::NEQ:           return instruction_kind::NE;
        case syntax::token_type_t::LT:            return instruction_kind::LT;
        case syntax::token_type_t::LT_EQ:         return instruction_kind::LE;
        case syntax::token_type_t::GT:            return instruction_kind::GT;
        case syntax::token_type_t::GT_EQ:         return instruction_kind::GE;
        default:                                  return stdx::none;
        }
    }

    [[nodiscard]] static constexpr auto map_unary_op(syntax::token_type_t tok) noexcept
        -> stdx::option<instruction_kind> {
        switch (tok) {
        case syntax::token_type_t::MINUS:         return instruction_kind::NEG;
        case syntax::token_type_t::MINUS_PERCENT: return instruction_kind::NEG;
        case syntax::token_type_t::BANG:          return instruction_kind::NOT;
        case syntax::token_type_t::NOT:           return instruction_kind::BITNOT;
        default:                                  return stdx::none;
        }
    }

    [[nodiscard]] auto active_mod(this auto&& self) noexcept -> auto& {
        return *self.active_module_;
    }
    [[nodiscard]] auto active_ast(this auto&& self) noexcept -> auto& {
        return self.active_module_->ast;
    }

    // The symbol table that owns a definition currently being emitted: the enclosing aggregate
    // literal's table for a member, otherwise the active module's root table.
    [[nodiscard]] auto current_owner_table_idx() const noexcept -> usize {
        if (!user_type_stack_.empty()) { return user_type_stack_.back()->get_symbol_table_idx(); }
        return *active_module_->root_table_idx;
    }

    // The GIR name a definition named `bare` in the current owner scope should be emitted under.
    [[nodiscard]] auto def_symbol_name(std::string_view bare) const -> std::string {
        return symbol_scoping_.name_for(current_owner_table_idx(), bare);
    }

    // The GIR name a reference at `ref_id` should target, honouring a resolver-recorded owner.
    [[nodiscard]] auto ref_symbol_name(ast::node_id ref_id, std::string_view bare) const
        -> std::string {
        if (const auto owner{active_module_->get_resolved_symbol_owner_opt(ref_id)}) {
            return symbol_scoping_.name_for(*owner, bare);
        }
        return std::string{bare};
    }

  private:
    sema::context&                ctx_;
    mod::module&                  ast_module_;
    stdx::option<mod::module&>    active_module_{ast_module_};
    const_eval                    const_eval_;
    builder                       builder_;
    module                        gir_module_;
    std::vector<scope_frame>      scopes_;
    std::vector<loop_context>     loop_stack_;
    default_counter               anon_test_desc_counter_;
    default_counter               anon_test_fn_counter_;
    default_counter               anon_fn_counter_;
    std::vector<std::string>      open_fn_names_;
    std::vector<bool>             open_fn_is_closure_;
    std::vector<sema::type*>      user_type_stack_;
    std::vector<std::string_view> pending_builtin_runtime_;
    symbol_scoping                symbol_scoping_;
    bool                          runtime_safety_{true};
    u32                           target_ptr_bits_{64};

    // While emitting an inherited interface default-method body: bare `self.method(...)` calls
    // are rewritten to target this impl's own methods (its body symbol table).
    stdx::opt_size emitting_impl_default_scope_;
    // For emitting a method body written directly in an `impl` block
    stdx::opt_size emitting_impl_body_scope_;
};

} // namespace ghoti::gir
