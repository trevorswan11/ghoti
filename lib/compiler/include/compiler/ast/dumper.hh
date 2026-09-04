#pragma once

#include <concepts>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/ostream.h>
#include <stdx/assert.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "support/indent.hh"

namespace ghoti::ast {

class dumper {
  public:
    explicit dumper(const AST& ast, std::ostream& out) : out_{out}, ast_{ast} {}

    auto dump() -> void {
        for (const auto& node : ast_) { dump(node); }
    }

    [[nodiscard]] static auto compare_source_asts(std::string_view s1, std::string_view s2) -> bool;

  private:
    template <IndexableNodeID ID> auto dump(ID id) -> void {
        ast_[id].visit([&](const auto& data) -> void { this->visit(id, data); });
    }

    auto dump(explicit_type_id id) -> void {
        fmt::println(out_, "ExplicitType (modifier: {})", id.get_modifier());
        const indent::guard g{indent_, true};
        ast_[id].visit([&](const auto& data) -> void { visit(id, data); });
    }

    auto visit(node_id, const array_expr&) -> void;
    auto visit(node_id, const asm_expr&) -> void;
    auto visit(node_id, const call_expr&) -> void;
    auto visit(node_id, const do_while_loop_expr&) -> void;
    auto visit(node_id, const enum_expr&) -> void;
    auto visit(node_id, const for_loop_expr&) -> void;
    auto visit(node_id, const function_expr&) -> void;
    auto visit(node_id, const identifier_expr&) -> void;
    auto visit(node_id, const if_expr&) -> void;
    auto visit(node_id, const index_expr&) -> void;
    auto visit(node_id, const infinite_loop_expr&) -> void;
    auto visit(node_id, const assignment_expr&) -> void;
    auto visit(node_id, const binary_expr&) -> void;
    auto visit(node_id, const dot_expr&) -> void;
    auto visit(node_id, const range_expr&) -> void;
    auto visit(node_id, const initializer_expr&) -> void;
    auto visit(node_id, const label_expr&) -> void;
    auto visit(node_id, const match_expr&) -> void;
    auto visit(node_id, const reference_expr&) -> void;
    auto visit(node_id, const address_of_expr&) -> void;
    auto visit(node_id, const dereference_expr&) -> void;
    auto visit(node_id, const unary_expr&) -> void;
    auto visit(node_id, const unwrap_expr&) -> void;
    auto visit(node_id, const implicit_access_expr&) -> void;
    auto visit(node_id, const string_expr&) -> void;
    auto visit(node_id, const i32_expr&) -> void;
    auto visit(node_id, const i64_expr&) -> void;
    auto visit(node_id, const isize_expr&) -> void;
    auto visit(node_id, const u32_expr&) -> void;
    auto visit(node_id, const u64_expr&) -> void;
    auto visit(node_id, const usize_expr&) -> void;
    auto visit(node_id, const u8_expr&) -> void;
    auto visit(node_id, const f32_expr&) -> void;
    auto visit(node_id, const f64_expr&) -> void;
    auto visit(node_id, const bool_expr&) -> void;
    auto visit(node_id, const void_expr&) -> void;
    auto visit(node_id, const undefined_expr&) -> void;
    auto visit(node_id, const nullptr_expr&) -> void;
    auto visit(node_id, const unreachable_expr&) -> void;
    auto visit(node_id, const module_access_expr&) -> void;
    auto visit(node_id, const struct_expr&) -> void;
    auto visit(node_id, const union_expr&) -> void;
    auto visit(node_id, const interface_expr&) -> void;
    auto visit(node_id, const while_loop_expr&) -> void;
    auto visit(node_id, const cfg_value_expr&) -> void;
    auto visit(node_id, const block_stmt&) -> void;
    auto visit(node_id, const break_stmt&) -> void;
    auto visit(node_id, const cfg_stmt&) -> void;
    auto visit(node_id, const continue_stmt&) -> void;
    auto visit(node_id, const decl_stmt&) -> void;
    auto visit(node_id, const defer_stmt&) -> void;
    auto visit(node_id, const discard_stmt&) -> void;
    auto visit(node_id, const expr_stmt&) -> void;
    auto visit(node_id, const impl_stmt&) -> void;
    auto visit(node_id, const import_stmt&) -> void;
    auto visit(node_id, const return_stmt&) -> void;
    auto visit(node_id, const test_stmt&) -> void;
    auto visit(node_id, const using_stmt&) -> void;
    auto visit(node_id, stdx::monostate) -> void { fmt::println(out_, "<discarded>"); }

    auto visit(explicit_type_id, const identifier_expr&) -> void;
    auto visit(explicit_type_id, const module_access_expr&) -> void;
    auto visit(explicit_type_id, const dot_expr&) -> void;
    auto visit(explicit_type_id, const call_expr&) -> void;
    auto visit(explicit_type_id, const explicit_function_type&) -> void;
    auto visit(explicit_type_id, const explicit_type_id&) -> void;
    auto visit(explicit_type_id, const struct_expr&) -> void;
    auto visit(explicit_type_id, const enum_expr&) -> void;
    auto visit(explicit_type_id, const union_expr&) -> void;
    auto visit(explicit_type_id, const interface_expr&) -> void;
    auto visit(explicit_type_id, const explicit_array_type&) -> void;
    auto visit(explicit_type_id, const explicit_dyn_type&) -> void;

    template <typename T, typename Func> void dump_container(const T& container, Func&& func) {
        for (auto it{container.begin()}; it != container.end(); ++it) {
            indent::guard g{indent_, std::next(it) == container.end()};
            std::forward<Func>(func)(*it);
        }
    }

    template <typename T> void dump_node_list(const T& list) {
        dump_container(list, [this](const auto& node_handle) -> void {
            fmt::print(out_, "{}", indent_.current_branch());
            dump(*node_handle);
        });
    }

    // Compact dump of an aggregate's `@cfg` groups; Only enough for AST comparison.
    template <typename Group> void dump_cfg_groups(const std::vector<Group>& groups) {
        fmt::println(out_, "{}CfgGroups:", indent_.current_branch());
        dump_container(groups, [this](const Group& group) -> void {
            fmt::println(out_, "{}Group @ {}", indent_.current_branch(), group.position);
            dump_container(group.arms, [this](const typename Group::arm& arm) -> void {
                if (arm.predicate) {
                    fmt::print(out_, "{}Arm: ", indent_.current_branch());
                    dump(*arm.predicate);
                } else {
                    fmt::println(out_, "{}Else:", indent_.current_branch());
                }
                dump_container(arm.items, [this](const auto& item) -> void {
                    fmt::print(out_, "{}Item: ", indent_.current_branch());
                    if constexpr (std::same_as<std::decay_t<decltype(item)>, member_handle>) {
                        dump(*item);
                    } else {
                        dump(item.name);
                    }
                });
            });
        });
    }

    template <> void dump_node_list<member_list>(const member_list& list) {
        dump_container(list, [this](const member_handle& mem_handle) -> void {
            fmt::print(out_, "{}", indent_.current_branch());
            dump(*mem_handle);
        });
    }

  private:
    std::ostream& out_;
    const AST&    ast_;
    indent        indent_;
};

} // namespace ghoti::ast
