#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/pointers>
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

// Everything a `@cfg` predicate can observe about the build
struct cfg_context {
    codegen::target_options target_opts;
    codegen::target_facts   facts;
    optimize_mode           build_mode{optimize_mode::DEBUG};
    bool                    runtime_safety{true};
    bool                    testing{false};
    mod::module_manager&    modules;

    [[nodiscard]] static auto from(const context& ctx) -> cfg_context;
};

// Evaluates `@cfg` predicates and `@cfgValue` constants over one module's AST without changing it.
// The old pipeline's `cfg_pass` splices with it, and AstGen selects arms with it.
class cfg_evaluator {
  public:
    using cfg_value = cfgval::value;

    // `@cfgValue` constants mid-evaluation and `@cfg` arms chosen, shared with the evaluators a
    // cross-module reference starts
    struct shared_state {
        struct arm_choice {
            bool           selected{false};
            stdx::opt_size arm;
        };

        ankerl::unordered_dense::set<std::pair<gsl::not_null<const mod::module_t*>, usize>>
             in_progress;
        bool hit_cycle{false};
        ankerl::unordered_dense::map<std::pair<gsl::not_null<const mod::module_t*>, usize>,
                                     arm_choice>
            arms;
    };

  public:
    cfg_evaluator(const mod::module_t& module, const cfg_context& cx, diagnostics& diags);
    cfg_evaluator(const mod::module_t& module,
                  const cfg_context&   cx,
                  diagnostics&         diags,
                  shared_state&        shared);

    // Finds the module's `@cfgValue` constants, so predicates can name them
    auto gather_cfg_values(const std::vector<ast::node_id>& list) -> void;
    // Evaluates every module-scope `@cfgValue` constant once, reporting their errors
    auto resolve_all_cfg_values() -> void;

    // The value of a `@cfgValue(...)` node, or none after reporting why it has none
    auto resolve_cfg_value(ast::node_id node) -> stdx::option<cfg_value>;
    // The expression a `@cfgValue` node selects, or none for the predicate form or on error
    [[nodiscard]] auto chosen_expression(ast::node_id node) -> stdx::option<ast::expr_handle>;
    [[nodiscard]] auto eval_predicate(ast::expr_handle h) -> stdx::option<bool>;

    // The surviving arm of a `@cfg` chain, statement or aggregate group
    template <typename Arm>
    [[nodiscard]] auto select_arm_of(const std::vector<Arm>& arms) -> stdx::option<const Arm&> {
        for (const auto& arm : arms) {
            if (!arm.predicate) { return arm; } // bare `else`
            const auto taken{eval_predicate(*arm.predicate)};
            if (!taken) { return stdx::none; }
            if (*taken) { return arm; }
        }
        return stdx::none;
    }

    // Fires a selected arm's bare `@compileError(...)` statements
    auto fire_eager_compile_errors(const std::vector<ast::stmt_handle>& items) -> void;

    // Records each `@cfgValue` verdict on the module, as the old pipeline's later passes read it
    auto record_results_on(mod::module_t& module) noexcept -> void { recorded_on_.emplace(module); }

    [[nodiscard]] auto is_ok() const noexcept -> bool { return ok_; }

  private:
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

    // A module-scope `@cfgValue` constant
    struct cfg_decl {
        ast::node_id decl;
        ast::node_id value;
    };

  private:
    template <typename... Args>
    auto fail(ast::node_id at, error code, fmt::format_string<Args...> spec, Args&&... args)
        -> void {
        ok_ = false;
        diags_.emplace_back(
            fmt::format(spec, std::forward<Args>(args)...), code, module_.ast.location_of(at));
    }

    [[nodiscard]] auto is_cpu_has(const ast::call_expr& call) const -> bool;
    [[nodiscard]] auto cpu_has(ast::node_id at, const ast::call_expr& call)
        -> stdx::option<cfg_value>;
    [[nodiscard]] auto to_operand(const cfg_value& value) -> operand;
    [[nodiscard]] auto find_import(std::string_view name, bool public_only)
        -> stdx::option<mod::module_t&>;
    [[nodiscard]] auto taken_arm(ast::node_id id, const ast::cfg_stmt& cfg) -> stdx::opt_size;
    [[nodiscard]] auto find_import_in(const std::vector<ast::stmt_handle>& items,
                                      std::string_view                     name,
                                      bool public_only) -> stdx::option<mod::module_t&>;
    [[nodiscard]] auto resolve_foreign_value(ast::node_id dot) -> stdx::option<cfg_value>;
    [[nodiscard]] auto is_compile_error_call(ast::expr_handle h) -> bool;
    [[nodiscard]] auto check_guard_arm_types(ast::node_id node, const ast::cfg_value_expr& expr)
        -> bool;

    [[nodiscard]] auto atom_value_str(std::string_view atom) const -> std::string_view;
    [[nodiscard]] auto atom_value(std::string_view atom) -> cfg_value;
    [[nodiscard]] auto int_literal(ast::expr_handle h) -> stdx::option<i64>;
    [[nodiscard]] auto eval_term(ast::expr_handle h) -> stdx::option<cfg_value>;
    [[nodiscard]] auto classify_operand(ast::expr_handle h) -> operand;
    [[nodiscard]] auto eval_comparison(ast::node_id         at,
                                       syntax::token_type_t op,
                                       ast::expr_handle     lhs_h,
                                       ast::expr_handle     rhs_h) -> stdx::option<bool>;

    [[nodiscard]] auto compile_error_message(const ast::call_expr& call)
        -> stdx::option<std::string_view>;
    auto fire_compile_error(ast::node_id at, const ast::call_expr& call) -> void;

  private:
    const mod::module_t& module_;
    const cfg_context&   cx_;
    diagnostics&         diags_;
    bool                 ok_{true};
    // Resolved on the first `cpu.has(...)`
    stdx::option<codegen::target_cpu> target_cpu_;
    stdx::option<mod::module_t&>      recorded_on_;

    ankerl::unordered_dense::map<std::string_view, cfg_decl>        cfg_value_decls_;
    ankerl::unordered_dense::map<usize, cfg_value>                  cfg_value_cache_;
    ankerl::unordered_dense::map<usize, stdx::option<ast::node_id>> chosen_;
    shared_state                                                    own_shared_;
    gsl::not_null<shared_state*>                                    shared_{&own_shared_};
};

} // namespace ghoti::sema
