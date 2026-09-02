#pragma once

#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/doc.hh"

namespace ghoti::ast {

class formatter {
  public:
    explicit formatter(const AST&       ast,
                       std::ostream&    out,
                       u16              max_width     = 100,
                       u16              indent_spaces = 4,
                       std::string_view source        = {}) noexcept
        : out_{out}, ast_{ast}, max_width_{max_width}, indent_spaces_{indent_spaces},
          source_{source} {
        init_trivia();
    }

    auto format() -> void;

  private:
    struct comment_item {
        std::string_view text;
        usize            line;
        usize            col;
        bool             is_trailing{false};
        bool             is_leading_blank{false};
        bool             consumed{false};
    };

  private:
    template <IndexableID ID> auto format(ID id) -> syntax::doc_id {
        auto doc{ast_[id].visit([&](const auto& data) { return this->visit(id, data); })};
        if constexpr (IndexableNodeID<ID>) {
            for (u8 depth{ast_.paren_depth_of(id)}; depth != 0; --depth) {
                doc = doc_manager_.concat({doc_manager_.text("("), doc, doc_manager_.text(")")});
            }
        }
        return doc;
    }

    [[nodiscard]] auto with_modifier(explicit_type_id id, syntax::doc_id base) -> syntax::doc_id;
    [[nodiscard]] auto blank_line_between(node_id before, node_id after) const -> bool;
    [[nodiscard]] auto is_function_or_aggregate_node(node_id id) const -> bool;

    [[nodiscard]] auto format_struct(const struct_expr& node) -> syntax::doc_id;
    [[nodiscard]] auto format_union(const union_expr& node) -> syntax::doc_id;
    [[nodiscard]] auto format_enum(const enum_expr& node) -> syntax::doc_id;

    auto format_members(std::vector<syntax::doc_id>&                      entries,
                        const member_list&                                members,
                        const std::vector<cfg_item_group<member_handle>>& cfg_groups) -> void;

    [[nodiscard]] auto format_member_cfg_group(const cfg_item_group<member_handle>& group)
        -> syntax::doc_id;
    [[nodiscard]] auto aggregate_body(std::vector<syntax::doc_id> entries, usize comma_count)
        -> syntax::doc_id;

    template <typename Group, typename ItemFmt>
    [[nodiscard]] auto format_aggregate_cfg_group(const Group& group, ItemFmt item_fmt)
        -> syntax::doc_id {
        std::vector<syntax::doc_id> parts;
        for (usize a{0}; a < group.arms.size(); ++a) {
            const auto& arm{group.arms[a]};
            if (a != 0) { parts.emplace_back(doc_manager_.text(" else ")); }
            if (arm.predicate) {
                parts.emplace_back(doc_manager_.text("@cfg ("));
                parts.emplace_back(format(*arm.predicate));
                parts.emplace_back(doc_manager_.text(") "));
            }
            std::vector<syntax::doc_id> items;
            items.reserve(arm.items.size());
            for (const auto& item : arm.items) { items.emplace_back(item_fmt(item)); }
            parts.emplace_back(doc_manager_.delimited("{", "}", std::move(items), true, true));
        }
        return doc_manager_.concat(std::move(parts));
    }

    [[nodiscard]] auto decl_prefix(const decl_stmt& node) -> syntax::doc_id;
    [[nodiscard]] auto tail_clause(node_id stmt) -> syntax::doc_id;

    auto visit(node_id, const array_expr&) -> syntax::doc_id;
    auto visit(node_id, const asm_expr&) -> syntax::doc_id;
    auto visit(node_id, const call_expr&) -> syntax::doc_id;
    auto visit(node_id, const do_while_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const enum_expr&) -> syntax::doc_id;
    auto visit(node_id, const for_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const function_expr&) -> syntax::doc_id;
    auto visit(node_id, const identifier_expr&) -> syntax::doc_id;
    auto visit(node_id, const if_expr&) -> syntax::doc_id;
    auto visit(node_id, const index_expr&) -> syntax::doc_id;
    auto visit(node_id, const infinite_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const assignment_expr&) -> syntax::doc_id;
    auto visit(node_id, const binary_expr&) -> syntax::doc_id;
    auto visit(node_id, const dot_expr&) -> syntax::doc_id;
    auto visit(node_id, const range_expr&) -> syntax::doc_id;
    auto visit(node_id, const initializer_expr&) -> syntax::doc_id;
    auto visit(node_id, const label_expr&) -> syntax::doc_id;
    auto visit(node_id, const match_expr&) -> syntax::doc_id;
    auto visit(node_id, const reference_expr&) -> syntax::doc_id;
    auto visit(node_id, const address_of_expr&) -> syntax::doc_id;
    auto visit(node_id, const dereference_expr&) -> syntax::doc_id;
    auto visit(node_id, const unary_expr&) -> syntax::doc_id;
    auto visit(node_id, const unwrap_expr&) -> syntax::doc_id;
    auto visit(node_id, const implicit_access_expr&) -> syntax::doc_id;
    auto visit(node_id, const string_expr&) -> syntax::doc_id;
    auto visit(node_id, const i32_expr&) -> syntax::doc_id;
    auto visit(node_id, const i64_expr&) -> syntax::doc_id;
    auto visit(node_id, const isize_expr&) -> syntax::doc_id;
    auto visit(node_id, const u32_expr&) -> syntax::doc_id;
    auto visit(node_id, const u64_expr&) -> syntax::doc_id;
    auto visit(node_id, const usize_expr&) -> syntax::doc_id;
    auto visit(node_id, const u8_expr&) -> syntax::doc_id;
    auto visit(node_id, const f32_expr&) -> syntax::doc_id;
    auto visit(node_id, const f64_expr&) -> syntax::doc_id;
    auto visit(node_id, const bool_expr&) -> syntax::doc_id;
    auto visit(node_id, const void_expr&) -> syntax::doc_id;
    auto visit(node_id, const undefined_expr&) -> syntax::doc_id;
    auto visit(node_id, const nullptr_expr&) -> syntax::doc_id;
    auto visit(node_id, const unreachable_expr&) -> syntax::doc_id;
    auto visit(node_id, const module_access_expr&) -> syntax::doc_id;
    auto visit(node_id, const struct_expr&) -> syntax::doc_id;
    auto visit(node_id, const union_expr&) -> syntax::doc_id;
    auto visit(node_id, const while_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const cfg_value_expr&) -> syntax::doc_id;
    auto visit(node_id, const block_stmt&) -> syntax::doc_id;
    auto visit(node_id, const break_stmt&) -> syntax::doc_id;
    auto visit(node_id, const cfg_stmt&) -> syntax::doc_id;
    auto visit(node_id, const continue_stmt&) -> syntax::doc_id;
    auto visit(node_id, const decl_stmt&) -> syntax::doc_id;
    auto visit(node_id, const defer_stmt&) -> syntax::doc_id;
    auto visit(node_id, const discard_stmt&) -> syntax::doc_id;
    auto visit(node_id, const expr_stmt&) -> syntax::doc_id;
    auto visit(node_id, const import_stmt&) -> syntax::doc_id;
    auto visit(node_id, const return_stmt&) -> syntax::doc_id;
    auto visit(node_id, const test_stmt&) -> syntax::doc_id;
    auto visit(node_id, const using_stmt&) -> syntax::doc_id;
    auto visit(node_id, stdx::monostate) -> syntax::doc_id;

    auto visit(explicit_type_id, const identifier_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const module_access_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const dot_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const call_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_function_type&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_type_id&) -> syntax::doc_id;
    auto visit(explicit_type_id, const struct_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const enum_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const union_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_array_type&) -> syntax::doc_id;

    auto init_trivia() -> void;
    auto consume_leading_comments(usize before_line, bool allow_leading_blank = false)
        -> syntax::doc_id;
    auto consume_trailing_comment(usize line) -> syntax::doc_id;
    auto consume_dangling_comments(usize brace_line) -> syntax::doc_id;
    auto consume_remaining_comments() -> syntax::doc_id;

  private:
    std::ostream&             out_;
    const AST&                ast_;
    syntax::doc_manager       doc_manager_;
    u16                       max_width_;
    u16                       indent_spaces_;
    std::string_view          source_;
    std::vector<comment_item> comments_;
    usize                     comment_idx_{0};
};

} // namespace ghoti::ast
