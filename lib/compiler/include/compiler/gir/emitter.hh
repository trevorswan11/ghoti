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
#include "compiler/gir/builder.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"
#include "support/scope_guard.hh"

namespace ghoti::gir {

class emitter {
  public:
    explicit emitter(sema::context& ctx, mod::module& ast_mod) noexcept
        : ctx_{ctx}, ast_module_{ast_mod}, const_eval_{ctx_, ast_mod},
          gir_module_{ast_mod, ctx_.arena} {}
    ~emitter() = default;
    MAKE_PINNED(emitter);

    // Translates the AST module into a GIR module
    [[nodiscard]] auto emit() -> module;

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
        local_id                       var_slot;
        stdx::option<sema::type&>      elem_type;
        value                          end_val{};
        stdx::option<std::string_view> capture_name;
        stdx::option<value>            arr_val{};
        stdx::option<sema::type&>      capture_type;
    };

    struct scope_frame {
        ankerl::unordered_dense::map<std::string_view, local_binding> bindings;
        std::vector<ast::stmt_handle>                                 defers;
    };

    using scope_guard        = ghoti::scope_guard<std::vector<scope_frame>>;
    using loop_context_guard = ghoti::scope_guard<std::vector<loop_context>>;

  private:
    auto emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto emit_top_level_using(ast::node_id id, const ast::using_stmt& using_stmt) -> void;
    auto emit_top_level_test(ast::node_id id, const ast::test_stmt& test) -> void;

    auto emit_function(ast::node_id              id,
                       const ast::decl_stmt&     decl,
                       const ast::function_expr& fn_expr) -> void;
    auto emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr) -> std::string;

    // Emits a capturing function_expr's implementation (once, idempotently) and constructs its
    // environment value at the current (definition-site) insertion point
    auto emit_closure(ast::node_id id, const ast::function_expr& fn_expr) -> value;
    auto emit_closure_function(const ast::function_expr&     fn_expr,
                               const sema::types::closure_t& cl,
                               sema::type&                   closure_type) -> void;
    auto emit_closure_env(const sema::types::closure_t& cl, sema::type& closure_type) -> value;

    // Reads a captured variable's current value (VALUE mode) or address (REF/MUT_REF mode) from
    // the definition-site scope, for use when constructing an environment field
    auto get_capture_source(const sema::types::closure_capture& capture) -> value;

    auto emit_stmt(const ast::stmt_handle& stmt) -> void;
    auto emit_block(const ast::block_stmt& block) -> void;
    auto emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto emit_return_stmt(ast::node_id id, const ast::return_stmt& ret) -> void;
    auto emit_defer_stmt(ast::node_id id, const ast::defer_stmt& def) -> void;
    auto emit_break(ast::node_id id, const ast::break_stmt& brk) -> void;
    auto emit_continue(ast::node_id id, const ast::continue_stmt& cnt) -> void;
    auto emit_stmt_as_value(const ast::stmt_handle& stmt) -> value;

    auto emit_defers_for_scope(usize scope_idx) -> void;
    auto emit_defers_up_to(usize target_depth) -> void;
    auto emit_lvalue(ast::node_id id) -> value;
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
    auto emit_coerced_expr(ast::expr_handle expr_id, const sema::type& dest_type) -> value;
    auto emit_generic_instantiation(const sema::generic_instantiation_request& req) -> void;
    auto emit_expression_id(ast::node_id id) -> value;
    // Produces a value exactly as resolved, including a bare reference-typed value where
    // applicable.
    auto emit_expression_id_raw(ast::node_id id) -> value;
    auto emit_if(ast::node_id id, const ast::if_expr& if_expr) -> value;
    auto emit_match(ast::node_id id, const ast::match_expr& match) -> value;
    auto emit_initializer(ast::node_id id, const ast::initializer_expr& init) -> value;
    auto emit_dot(ast::node_id id, const ast::dot_expr& dot) -> value;
    auto emit_index(ast::node_id id, const ast::index_expr& index) -> value;
    auto emit_address_of(ast::node_id id, const ast::address_of_expr& addr) -> value;
    auto emit_dereference(ast::node_id id, const ast::dereference_expr& deref) -> value;
    auto emit_reference(ast::node_id id, const ast::reference_expr& ref) -> value;
    auto emit_implicit_access(ast::node_id id, const ast::implicit_access_expr& imp) -> value;
    auto emit_module_access(ast::node_id id, const ast::module_access_expr& mod_access) -> value;
    auto emit_while(ast::node_id                   id,
                    const ast::while_loop_expr&    while_loop,
                    stdx::option<std::string_view> label    = stdx::none,
                    stdx::option<local_id>         res_slot = stdx::none) -> value;
    auto emit_do_while(ast::node_id                   id,
                       const ast::do_while_loop_expr& do_while,
                       stdx::option<std::string_view> label    = stdx::none,
                       stdx::option<local_id>         res_slot = stdx::none) -> value;
    auto emit_infinite_loop(ast::node_id                   id,
                            const ast::infinite_loop_expr& loop,
                            stdx::option<std::string_view> label    = stdx::none,
                            stdx::option<local_id>         res_slot = stdx::none) -> value;
    auto emit_for(ast::node_id                   id,
                  const ast::for_loop_expr&      for_loop,
                  stdx::option<std::string_view> label    = stdx::none,
                  stdx::option<local_id>         res_slot = stdx::none) -> value;
    auto emit_label(ast::node_id id, const ast::label_expr& label) -> value;
    auto emit_binary(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_logical_and(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_logical_or(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value;
    auto emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value;
    auto emit_call(ast::node_id id, const ast::call_expr& call) -> value;
    auto emit_ident(ast::node_id id, const ast::identifier_expr& ident) -> value;

    template <stdx::Reference Binding = const local_binding&>
    auto lookup_binding(std::string_view name) noexcept -> stdx::option<Binding> {
        PROFILE_FUNCTION();
        for (auto& frame : std::views::reverse(scopes_)) {
            if (auto it{frame.bindings.find(name)}; it != frame.bindings.end()) {
                return stdx::option<Binding>{it->second};
            }
        }
        return stdx::none;
    }

    [[nodiscard]] static constexpr auto map_binary_op(syntax::token_type_t tok) noexcept
        -> stdx::option<instruction_kind> {
        switch (tok) {
        case syntax::token_type_t::PLUS:    return instruction_kind::ADD;
        case syntax::token_type_t::MINUS:   return instruction_kind::SUB;
        case syntax::token_type_t::STAR:    return instruction_kind::MUL;
        case syntax::token_type_t::SLASH:   return instruction_kind::DIV;
        case syntax::token_type_t::PERCENT: return instruction_kind::MOD;
        case syntax::token_type_t::BW_AND:  return instruction_kind::AND;
        case syntax::token_type_t::BW_OR:   return instruction_kind::OR;
        case syntax::token_type_t::CARET:   return instruction_kind::XOR;
        case syntax::token_type_t::SHL:     return instruction_kind::SHL;
        case syntax::token_type_t::SHR:     return instruction_kind::SHR;
        case syntax::token_type_t::EQ:      return instruction_kind::EQ;
        case syntax::token_type_t::NEQ:     return instruction_kind::NE;
        case syntax::token_type_t::LT:      return instruction_kind::LT;
        case syntax::token_type_t::LT_EQ:   return instruction_kind::LE;
        case syntax::token_type_t::GT:      return instruction_kind::GT;
        case syntax::token_type_t::GT_EQ:   return instruction_kind::GE;
        default:                            return stdx::none;
        }
    }

    [[nodiscard]] static constexpr auto map_unary_op(syntax::token_type_t tok) noexcept
        -> stdx::option<instruction_kind> {
        switch (tok) {
        case syntax::token_type_t::MINUS: return instruction_kind::NEG;
        case syntax::token_type_t::BANG:  return instruction_kind::NOT;
        case syntax::token_type_t::NOT:   return instruction_kind::BITNOT;
        default:                          return stdx::none;
        }
    }

    [[nodiscard]] auto active_mod(this auto&& self) noexcept -> auto& {
        return *self.active_module_;
    }
    [[nodiscard]] auto active_ast(this auto&& self) noexcept -> auto& {
        return self.active_module_->ast;
    }

  private:
    sema::context&             ctx_;
    mod::module&               ast_module_;
    stdx::option<mod::module&> active_module_{ast_module_};
    const_eval                 const_eval_;
    builder                    builder_;
    module                     gir_module_;
    std::vector<scope_frame>   scopes_;
    std::vector<loop_context>  loop_stack_;
    default_counter            anon_test_counter_;
    default_counter            anon_fn_counter_;
};

} // namespace ghoti::gir
