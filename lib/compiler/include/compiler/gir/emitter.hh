#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/builder.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::gir {

class emitter {
  public:
    explicit emitter(sema::context& ctx, mod::module& ast_mod) noexcept
        : ctx_{ctx}, ast_module_{ast_mod}, const_eval_{ctx_, ast_mod}, gir_module_{ast_mod} {}
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
    };

    struct scope_frame {
        ankerl::unordered_dense::map<std::string_view, local_binding> bindings;
    };

    class scope_guard {
      public:
        scope_guard(std::vector<scope_frame>& scopes) noexcept : scopes_{scopes} {
            scopes_.emplace_back();
        }
        ~scope_guard() { scopes_.pop_back(); }
        MAKE_PINNED(scope_guard);

      private:
        std::vector<scope_frame>& scopes_;
    };

  private:
    auto emit_top_level_decl(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto emit_top_level_using(ast::node_id id, const ast::using_stmt& using_stmt) -> void;
    auto emit_top_level_test(ast::node_id id, const ast::test_stmt& test) -> void;

    auto emit_function(ast::node_id              id,
                       const ast::decl_stmt&     decl,
                       const ast::function_expr& fn_expr) -> void;
    auto emit_anonymous_function(ast::node_id id, const ast::function_expr& fn_expr) -> std::string;

    auto emit_stmt(const ast::stmt_handle& stmt) -> void;
    auto emit_block(const ast::block_stmt& block) -> void;
    auto emit_decl_stmt(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto emit_return_stmt(ast::node_id id, const ast::return_stmt& ret) -> void;

    auto emit_expression(const ast::expr_handle& expr) -> value {
        return emit_expression_id(*expr);
    }
    auto emit_expression_id(ast::node_id id) -> value;
    auto emit_binary(ast::node_id id, const ast::binary_expr& binary) -> value;
    auto emit_unary(ast::node_id id, const ast::unary_expr& unary) -> value;
    auto emit_assignment(ast::node_id id, const ast::assignment_expr& assign) -> value;
    auto emit_call(ast::node_id id, const ast::call_expr& call) -> value;
    auto emit_ident(ast::node_id id, const ast::identifier_expr& ident) -> value;

    auto lookup_binding(std::string_view name) const noexcept -> stdx::option<const local_binding&>;

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

  private:
    sema::context&           ctx_;
    mod::module&             ast_module_;
    const_eval               const_eval_;
    builder                  builder_;
    module                   gir_module_;
    std::vector<scope_frame> scopes_;
    usize                    anon_test_counter_{0};
    usize                    anon_fn_counter_{0};
};

} // namespace ghoti::gir
