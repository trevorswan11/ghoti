#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/doc.hh"
#include "support/diagnostic.hh"

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
        bool             is_trailing_blank{false}; // a blank line before the token after it
        bool             consumed{false};
    };

    // A `delimited` list's items with their comments, `trailers` lining up with `items`
    struct commented_items {
        std::vector<syntax::doc_id> items;
        std::vector<syntax::doc_id> trailers;
        bool                        has_comments{false};
    };

    // A `// ghoti fmt: off` region over a run of list items, kept as its original source lines
    struct verbatim_region {
        syntax::doc_id doc;      // leading comments, the author's blank line, then the source
        usize          items;    // list items it covers, starting at the one it was found before
        usize          end_line; // last source line it covers
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
    [[nodiscard]] auto format_callconv(calling_convention conv, bool is_explicit) -> syntax::doc_id;
    [[nodiscard]] auto blank_line_between(node_id before, node_id after) const -> bool;
    // As `blank_line_between`, but measured from the end of an off region when `previous` closed
    // one
    [[nodiscard]] auto blank_line_after(node_id                    previous,
                                        const stdx::option<usize>& region_end,
                                        node_id                    next) const -> bool;
    [[nodiscard]] auto is_function_or_aggregate_node(node_id id) const -> bool;

    // Shared layout of `struct` / `union`; `keyword` is the head before the body, like `struct `
    template <typename Node, typename FieldItem, typename FieldEndLine>
    auto format_field_aggregate(const Node&    node,
                                syntax::doc_id keyword,
                                FieldItem&&    field_item,
                                FieldEndLine&& field_end_line) -> syntax::doc_id;

    [[nodiscard]] auto format_struct(const struct_expr& node) -> syntax::doc_id;
    [[nodiscard]] auto format_union(const union_expr& node) -> syntax::doc_id;
    [[nodiscard]] auto format_enum(const enum_expr& node) -> syntax::doc_id;
    [[nodiscard]] auto format_interface(const interface_expr& node) -> syntax::doc_id;

    auto format_members(std::vector<syntax::doc_id>&                      entries,
                        const member_list&                                members,
                        const std::vector<cfg_item_group<member_handle>>& cfg_groups) -> void;

    [[nodiscard]] auto format_member_cfg_group(const cfg_item_group<member_handle>& group,
                                               bool allow_leading_blank) -> syntax::doc_id;
    [[nodiscard]] auto aggregate_body(std::vector<syntax::doc_id> entries,
                                      usize                       comma_count,
                                      bool force_break = false) -> syntax::doc_id;

    // A field or variant `@cfg` group, `item_start` / `item_end` giving an item's source lines
    template <typename Group, typename ItemFmt, typename ItemStart, typename ItemEnd>
    [[nodiscard]] auto format_aggregate_cfg_group(const Group& group,
                                                  bool         allow_leading_blank,
                                                  ItemFmt&&    item_fmt,
                                                  ItemStart&&  item_start,
                                                  ItemEnd&&    item_end) -> syntax::doc_id;

    // `a, b, c => ` heading a match or `@cfgValue` arm, `force_break` from a trailing comma
    [[nodiscard]] auto alternatives_head(std::vector<syntax::doc_id> alternatives, bool force_break)
        -> syntax::doc_id;
    // `@cfg (pred) ` before an arm, or ` else ` / ` else @cfg (pred) ` after the first
    [[nodiscard]] auto cfg_arm_head(bool first, const stdx::option<expr_handle>& predicate)
        -> syntax::doc_id;
    // The comments of a body-less braced arm, or `{}` when it has none
    [[nodiscard]] auto empty_cfg_arm(const stdx::option<source_location>& close) -> syntax::doc_id;
    // Surrounds a `@cfg` group with the comments before its first line and after its last
    template <typename Group, typename Render>
    [[nodiscard]] auto with_cfg_group_comments(const Group& group,
                                               usize        end_line,
                                               bool         allow_leading_blank,
                                               Render&&     render) -> syntax::doc_id;

    [[nodiscard]] auto decl_prefix(const decl_stmt& node) -> syntax::doc_id;
    [[nodiscard]] auto attribute_list_doc(const attribute_list& list) -> syntax::doc_id;

    // Keeps `@[...]` beside `head` when both fit on one line, otherwise puts it on its own line
    [[nodiscard]] auto with_attributes(const stdx::option<attribute_list>& attributes,
                                       syntax::doc_id                      head) -> syntax::doc_id;
    [[nodiscard]] auto tail_clause(node_id stmt) -> syntax::doc_id;
    // ` = value` for an `=` on `operator_line`, with a multiline string value starting its own
    // line so its lines align
    [[nodiscard]] auto assigned_value(usize operator_line, node_id value) -> syntax::doc_id;
    // `|v|` / `|&mut v|`, with a trailing space when a body follows on the same line
    [[nodiscard]] auto format_capture(const capture_t& capture, bool trailing_space = false)
        -> syntax::doc_id;

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
    // The name after a `.`
    auto member_name(identifier_handle member) -> syntax::doc_id;
    auto visit(node_id, const range_expr&) -> syntax::doc_id;
    auto visit(node_id, const initializer_expr&) -> syntax::doc_id;
    auto visit(node_id, const label_expr&) -> syntax::doc_id;
    auto visit(node_id, const match_expr&) -> syntax::doc_id;
    auto visit(node_id, const reference_expr&) -> syntax::doc_id;
    auto visit(node_id, const address_of_expr&) -> syntax::doc_id;
    auto visit(node_id, const dereference_expr&) -> syntax::doc_id;
    auto visit(node_id, const comptime_expr&) -> syntax::doc_id;
    auto visit(node_id, const unary_expr&) -> syntax::doc_id;
    auto visit(node_id, const unwrap_expr&) -> syntax::doc_id;
    auto visit(node_id, const implicit_access_expr&) -> syntax::doc_id;
    auto visit(node_id, const string_expr&) -> syntax::doc_id;
    auto visit(node_id, const int_literal_expr&) -> syntax::doc_id;
    auto visit(node_id, const float_literal_expr&) -> syntax::doc_id;
    auto visit(node_id, const bool_expr&) -> syntax::doc_id;
    auto visit(node_id, const void_expr&) -> syntax::doc_id;
    auto visit(node_id, const undefined_expr&) -> syntax::doc_id;
    auto visit(node_id, const nullptr_expr&) -> syntax::doc_id;
    auto visit(node_id, const unreachable_expr&) -> syntax::doc_id;
    auto visit(node_id, const type_expr&) -> syntax::doc_id;
    auto visit(node_id, const struct_expr&) -> syntax::doc_id;
    auto visit(node_id, const union_expr&) -> syntax::doc_id;
    auto visit(node_id, const interface_expr&) -> syntax::doc_id;
    auto visit(node_id, const while_loop_expr&) -> syntax::doc_id;
    auto visit(node_id, const cfg_value_expr&) -> syntax::doc_id;
    auto visit(node_id, const block_stmt&) -> syntax::doc_id;
    auto visit(node_id, const break_stmt&) -> syntax::doc_id;
    auto visit(node_id, const cfg_stmt&) -> syntax::doc_id;
    auto visit(node_id, const continue_stmt&) -> syntax::doc_id;
    auto visit(node_id, const decl_stmt&) -> syntax::doc_id;
    auto visit(node_id, const defer_stmt&) -> syntax::doc_id;
    auto visit(node_id, const errdefer_stmt&) -> syntax::doc_id;
    auto visit(node_id, const discard_stmt&) -> syntax::doc_id;
    auto visit(node_id, const expr_stmt&) -> syntax::doc_id;
    auto visit(node_id, const impl_stmt&) -> syntax::doc_id;
    auto visit(node_id, const import_stmt&) -> syntax::doc_id;
    auto visit(node_id, const return_stmt&) -> syntax::doc_id;
    auto visit(node_id, const test_stmt&) -> syntax::doc_id;
    auto visit(node_id, stdx::monostate) -> syntax::doc_id;

    auto visit(explicit_type_id, const identifier_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const dot_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const call_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_function_type&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_type_id&) -> syntax::doc_id;
    auto visit(explicit_type_id, const struct_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const enum_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const union_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const interface_expr&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_array_type&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_dyn_type&) -> syntax::doc_id;
    auto visit(explicit_type_id, const explicit_expr_type&) -> syntax::doc_id;

    auto init_trivia() -> void;
    auto consume_leading_comments(usize before_line, bool allow_leading_blank = false)
        -> syntax::doc_id;
    auto consume_trailing_comment(usize line) -> syntax::doc_id;
    // Comments before the closing brace at (`brace_line`, `brace_col`); one after it is trailing
    auto consume_dangling_comments(usize brace_line, usize brace_col = SIZE_MAX) -> syntax::doc_id;
    auto consume_remaining_comments() -> syntax::doc_id;
    // The comment ending `operator_line` when the operand `rhs` starts below it, as in `a + // c`
    auto consume_operator_comment(usize operator_line, node_id rhs) -> syntax::doc_id;
    // What separates an operator from its operand: `line()`, or the operator's comment and a
    // hard break
    auto operand_break(syntax::doc_id operator_comment) -> syntax::doc_id;

    // Index of the first unconsumed own-line `// ghoti fmt: <state>` comment before `before_line`
    [[nodiscard]] auto find_fmt_marker(usize from, usize before_line, std::string_view state) const
        -> stdx::option<usize>;
    // Lines `first` through `last` of the source, without the final line break
    [[nodiscard]] auto source_lines(usize first, usize last) const -> std::string_view;

    // Takes the off region opening before list item `first`, if any. It covers items up to
    // `boundary` that start before its `// ghoti fmt: on`, and reaches that marker when it sits
    // before the next item, or before `limit_line` once the region runs to `boundary`
    template <typename StartLine, typename EndLine>
    auto take_verbatim_region(usize       first,
                              usize       boundary,
                              StartLine&& start_line,
                              EndLine&&   end_line,
                              usize       limit_line,
                              bool        allow_leading_blank) -> stdx::option<verbatim_region>;
    // Renders `count` comma-separated items that close on `close_line`. Each keeps the own-line
    // comments above it, the comment ending its line goes after its comma, and comments left
    // before the closing delimiter follow the last item.
    template <typename StartLine, typename EndLine, typename Render>
    auto format_commented_items(
        usize count, usize close_line, StartLine&& start_line, EndLine&& end_line, Render&& render)
        -> commented_items;
    // `delimited` over commented items, broken whenever any comment is kept
    auto commented_delimited(std::string_view open,
                             std::string_view close,
                             commented_items  list,
                             bool             pad,
                             bool             trailing_comma,
                             bool             force_break) -> syntax::doc_id;
    // A parameter's type, with an `impl` bound in place of the `auto` it desugared to
    auto format_param_type(explicit_type_id type, syntax::doc_id bound) -> syntax::doc_id;

  private:
    std::ostream&             out_;
    const AST&                ast_;
    syntax::doc_manager       doc_manager_;
    u16                       max_width_;
    u16                       indent_spaces_;
    std::string_view          source_;
    std::vector<comment_item> comments_;
    usize                     comment_idx_{0};
    std::vector<usize>        line_starts_;
    // An `impl` parameter's bound, rendered where its desugared `auto` sits (`[]impl I`)
    stdx::option<syntax::doc_id> auto_replacement_;
};

} // namespace ghoti::ast
