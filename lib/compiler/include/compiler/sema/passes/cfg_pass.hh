#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::sema {

namespace cfgval {

struct member {
    std::string_view name;
    auto             operator==(const member&) const -> bool = default;
};

struct text {
    std::string_view value;
    auto             operator==(const text&) const -> bool = default;
};

// A `@compileError` used as a `@cfgValue` guard value
struct diverges {
    auto operator==(const diverges&) const -> bool = default;
};

using value = stdx::variant<bool, i64, member, text, diverges>;

} // namespace cfgval

// Technically a preprocessor but not really, run it before symbol collection
class cfg_pass {
  public:
    [[nodiscard]] static auto run(mod::module& module, context& ctx) -> bool;

  private:
    using cfg_value = cfgval::value;

    // One side of a cfg comparison, after light classification
    struct operand {
        enum class kind : u8 {
            ATOM,
            PTR_BITS,
            MEMBER,
            INT,
            BOOL,
            STRING,
            ERROR,
        };
        kind             tag{kind::ERROR};
        std::string_view text{}; // atom or member name
        i64              integer{0};
        bool             boolean{false};
    };

  private:
    cfg_pass(mod::module& module, context& ctx)
        : module_{module}, ctx_{ctx},
          facts_{codegen::target_facts::resolve(ctx.target_opts.triple_str)} {}

    template <typename... Args>
    auto fail(ast::node_id at, error code, fmt::format_string<Args...> spec, Args&&... args)
        -> void {
        ok_ = false;
        ctx_.diags.emplace_back(
            fmt::format(spec, std::forward<Args>(args)...), code, module_.ast.location_of(at));
    }

    auto               gather_cfg_values(const std::vector<ast::node_id>& list) -> void;
    auto               resolve_cfg_value(ast::node_id node) -> stdx::option<cfg_value>;
    [[nodiscard]] auto is_compile_error_call(ast::expr_handle h) -> bool;
    [[nodiscard]] auto check_guard_arm_types(ast::node_id node, const ast::cfg_value_expr& expr)
        -> bool;

    [[nodiscard]] auto atom_value_str(std::string_view atom) const -> std::string_view;
    [[nodiscard]] auto atom_value(std::string_view atom) -> cfg_value;
    [[nodiscard]] auto int_literal(ast::expr_handle h) -> stdx::option<i64>;
    [[nodiscard]] auto eval_term(ast::expr_handle h) -> stdx::option<cfg_value>;
    [[nodiscard]] auto eval_predicate(ast::expr_handle h) -> stdx::option<bool>;
    [[nodiscard]] auto classify_operand(ast::expr_handle h) -> operand;
    [[nodiscard]] auto eval_comparison(ast::node_id         at,
                                       syntax::token_type_t op,
                                       ast::expr_handle     lhs_h,
                                       ast::expr_handle     rhs_h) -> stdx::option<bool>;

    [[nodiscard]] auto compile_error_message(const ast::call_expr& call)
        -> stdx::option<std::string_view>;
    auto fire_compile_error(ast::node_id at, const ast::call_expr& call) -> void;
    auto fire_eager_compile_errors(const std::vector<ast::stmt_handle>& items) -> void;

    [[nodiscard]] auto select_arm(const ast::cfg_stmt& cfg) -> const ast::cfg_stmt::arm*;
    auto               rewrite_items(std::vector<ast::stmt_handle>& list) -> void;
    auto               rewrite_roots(std::vector<ast::node_id>& roots) -> void;
    auto               recurse_into_bodies(ast::stmt_handle stmt) -> void;
    auto               recurse_into_expr(ast::expr_handle expr) -> void;
    auto               recurse_into_block(ast::block_handle block) -> void;

  private:
    mod::module&          module_;
    context&              ctx_;
    codegen::target_facts facts_;
    bool                  ok_{true};

    ankerl::unordered_dense::map<std::string_view, ast::node_id> cfg_value_decls_;
    ankerl::unordered_dense::map<usize, cfg_value>               cfg_value_cache_;
    ankerl::unordered_dense::set<usize>                          in_progress_;
};

} // namespace ghoti::sema
