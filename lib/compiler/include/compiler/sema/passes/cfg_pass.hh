#pragma once

#include <vector>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/context.hh"

namespace ghoti::sema {

// Splices each `@cfg` chain's selected arm into the AST, before symbol collection in the old
// pipeline. The predicates themselves are `cfg_evaluator`'s.
class cfg_pass {
  public:
    [[nodiscard]] static auto run(mod::module_t& module, context& ctx) -> bool;

  private:
    cfg_pass(mod::module_t& module, context& ctx);

    // Splices each aggregate `@cfg` group's selected items into `items` at the group's position
    template <typename Group, typename Item>
    auto flatten_cfg_groups(std::vector<Group>& groups, std::vector<Item>& items) -> void {
        usize offset{0};
        for (auto& group : groups) {
            const auto arm{eval_.select_arm_of(group.arms)};
            if (!arm) { continue; }
            const auto at{static_cast<idiff>(group.position + offset)};
            items.insert(items.begin() + at, arm->items.begin(), arm->items.end());
            offset += arm->items.size();
        }
        groups.clear();
    }

    auto rewrite_items(std::vector<ast::stmt_handle>& list) -> void;
    auto rewrite_roots(std::vector<ast::node_id>& roots) -> void;
    auto recurse_into_bodies(ast::stmt_handle stmt) -> void;
    auto recurse_into_expr(ast::expr_handle expr) -> void;
    auto recurse_into_block(ast::block_handle block) -> void;
    // Signature types can be expressions (`fn(): if (c) A else B`) holding `@cfg` blocks
    auto recurse_into_type(ast::explicit_type_id type) -> void;
    auto recurse_into_members(const ast::member_list& members) -> void;

  private:
    mod::module_t& module_;
    cfg_context    cx_;
    cfg_evaluator  eval_;
};

} // namespace ghoti::sema
