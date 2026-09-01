#include "compiler/ast/formatter.hh"

#include <cctype>
#include <cstddef>
#include <stdx/assert.hh>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"
#include "compiler/syntax/lexer.hh"
#include "compiler/syntax/operators.hh"
#include "compiler/syntax/token_type.hh"
#include "compiler/syntax/trvia.hh"

namespace ghoti::ast {

namespace {

auto operator_spelling(syntax::token_type_t tt) -> std::string_view {
    const auto spelling{syntax::get_operator_opt(tt)};
    ASSERT(spelling, "Unrecognized operator");
    return *spelling;
}

auto modifier_prefix(type_modifier mod) -> std::string_view {
    using m = type_modifier::modifier;
    switch (mod.get_raw()) {
    case m::VALUE:        return "";
    case m::REF:          return "&";
    case m::MUT_REF:      return "&mut ";
    case m::PTR:          return "^";
    case m::MUT_PTR:      return "^mut ";
    case m::VOLATILE:     return "volatile ";
    case m::MUT_VOLATILE: return "mut volatile ";
    default:              UNREACHABLE("Unrecognized type modifier");
    }
}

auto asm_option_spelling(asm_expr::option opt) -> std::string_view {
    switch (opt) {
    case asm_expr::option::VOLATILE:    return "volatile";
    case asm_expr::option::NORETURN:    return "noreturn";
    case asm_expr::option::INTEL:       return "intel";
    case asm_expr::option::ATT:         return "att";
    case asm_expr::option::ALIGN_STACK: return "align_stack";
    default:                            UNREACHABLE("Unrecognized asm option"); ;
    }
}

[[nodiscard]] auto prefix_needs_gap(std::string_view op) -> bool {
    return !op.empty() && (std::isalnum(static_cast<u8>(op.back())) != 0);
}

} // namespace

auto formatter::init_trivia() -> void {
    if (source_.empty()) { return; }

    syntax::lexer lex{source_};
    usize         prev_line{0};
    bool          first{true};

    while (true) {
        auto enriched{lex.advance_enriched()};

        for (const auto& t : enriched.leading_trivia) {
            if (t.kind == syntax::trivia_kind::LINE_COMMENT) {
                bool has_blank{false};
                if (!first && t.line > prev_line + 1) { has_blank = true; }
                comments_.emplace_back<comment_item>({
                    .text             = t.slice,
                    .line             = t.line,
                    .col              = t.col,
                    .is_trailing      = false,
                    .is_leading_blank = has_blank,
                    .consumed         = false,
                });
                prev_line = t.line;
                first     = false;
            }
        }

        if (enriched.token.type == syntax::token_type_t::END) { break; }

        prev_line = enriched.token.line;
        first     = false;

        for (const auto& t : enriched.trailing_trivia) {
            if (t.kind == syntax::trivia_kind::LINE_COMMENT) {
                comments_.emplace_back<comment_item>({
                    .text             = t.slice,
                    .line             = t.line,
                    .col              = t.col,
                    .is_trailing      = true,
                    .is_leading_blank = false,
                    .consumed         = false,
                });
                prev_line = t.line;
            }
        }
    }
}

auto formatter::consume_leading_comments(usize before_line) -> syntax::doc_id {
    if (comment_idx_ >= comments_.size()) { return doc_manager_.nil(); }

    std::vector<syntax::doc_id> docs;
    while (comment_idx_ < comments_.size()) {
        const auto& c{comments_[comment_idx_]};
        if (c.line >= before_line) { break; }
        if (!c.consumed) {
            comments_[comment_idx_].consumed = true;
            if (c.is_leading_blank && !docs.empty()) {
                docs.emplace_back(doc_manager_.hard_line());
            }
            docs.emplace_back(doc_manager_.text(c.text));
            docs.emplace_back(doc_manager_.hard_line());
        }
        ++comment_idx_;
    }
    if (docs.empty()) { return doc_manager_.nil(); }
    return doc_manager_.concat(std::move(docs));
}

auto formatter::consume_trailing_comment(usize line) -> syntax::doc_id {
    for (usize i{comment_idx_}; i < comments_.size(); ++i) {
        auto& c{comments_[i]};
        if (c.line > line) { break; }
        if (c.line == line && c.is_trailing && !c.consumed) {
            c.consumed = true;
            while (comment_idx_ < comments_.size() && comments_[comment_idx_].consumed) {
                ++comment_idx_;
            }
            return doc_manager_.concat({doc_manager_.text(" "), doc_manager_.text(c.text)});
        }
    }
    return doc_manager_.nil();
}

auto formatter::consume_dangling_comments(usize brace_line) -> syntax::doc_id {
    if (comment_idx_ >= comments_.size()) { return doc_manager_.nil(); }

    std::vector<syntax::doc_id> docs;
    while (comment_idx_ < comments_.size()) {
        const auto& c{comments_[comment_idx_]};
        if (c.line > brace_line) { break; }
        if (!c.consumed) {
            comments_[comment_idx_].consumed = true;
            if (!docs.empty()) {
                docs.emplace_back(doc_manager_.hard_line());
                if (c.is_leading_blank) { docs.emplace_back(doc_manager_.hard_line()); }
            }
            docs.emplace_back(doc_manager_.text(c.text));
        }
        ++comment_idx_;
    }
    if (docs.empty()) { return doc_manager_.nil(); }
    return doc_manager_.concat(std::move(docs));
}

auto formatter::consume_remaining_comments() -> syntax::doc_id {
    if (comment_idx_ >= comments_.size()) { return doc_manager_.nil(); }

    std::vector<syntax::doc_id> docs;
    while (comment_idx_ < comments_.size()) {
        const auto& c{comments_[comment_idx_]};
        if (!c.consumed) {
            comments_[comment_idx_].consumed = true;
            if (c.is_leading_blank) { docs.emplace_back(doc_manager_.hard_line()); }
            docs.emplace_back(doc_manager_.text(c.text));
            docs.emplace_back(doc_manager_.hard_line());
        }
        ++comment_idx_;
    }
    if (docs.empty()) { return doc_manager_.nil(); }
    return doc_manager_.concat(std::move(docs));
}

auto formatter::format() -> void {
    auto previous{node_id::make_invalid()};
    for (const auto id : ast_) {
        const auto& start_loc{ast_.location_of(id)};
        const auto& end_loc{ast_.end_location_of(id)};

        auto leading{consume_leading_comments(start_loc.line)};
        if (leading != doc_manager_.nil()) { doc_manager_.add_root(leading); }

        if (previous.is_valid() &&
            (blank_line_between(previous, id) || is_function_or_aggregate_node(previous))) {
            if (leading == doc_manager_.nil()) { doc_manager_.add_root(doc_manager_.hard_line()); }
        }

        auto node_doc{format(id)};
        auto trailing{consume_trailing_comment(end_loc.line)};
        if (trailing != doc_manager_.nil()) {
            node_doc = doc_manager_.concat({node_doc, trailing});
        }

        doc_manager_.add_root(node_doc);
        doc_manager_.add_root(doc_manager_.hard_line());
        previous = id;
    }

    auto remaining{consume_remaining_comments()};
    if (remaining != doc_manager_.nil()) { doc_manager_.add_root(remaining); }

    syntax::layout_engine solver{doc_manager_, max_width_, indent_spaces_};
    solver.render(out_);
}

auto formatter::with_modifier(explicit_type_id id, syntax::doc_id base) -> syntax::doc_id {
    const auto prefix{modifier_prefix(id.get_modifier())};
    if (prefix.empty()) { return base; }
    return doc_manager_.concat({doc_manager_.text(prefix), base});
}

auto formatter::blank_line_between(node_id before, node_id after) const -> bool {
    if (is_function_or_aggregate_node(before)) { return true; }
    return ast_.location_of(after).line > ast_.end_location_of(before).line + 1;
}

auto formatter::is_function_or_aggregate_node(node_id id) const -> bool {
    const auto is_aggregate = [this](auto id) {
        return ast_.get_as_opt<struct_expr>(id) || ast_.get_as_opt<union_expr>(id) ||
               ast_.get_as_opt<struct_expr>(id);
    };

    if (ast_.get_as_opt<test_stmt>(id)) { return true; }
    if (ast_.get_as_opt<function_expr>(id) || is_aggregate(id)) { return true; }
    if (const auto decl{ast_.get_as_opt<decl_stmt>(id)}) {
        if (decl->value &&
            (ast_.get_as_opt<function_expr>(*decl->value) || is_aggregate(*decl->value))) {
            return true;
        }
        if (decl->explicit_type && is_aggregate(*decl->explicit_type)) { return true; }
    }
    if (const auto expr{ast_.get_as_opt<expr_stmt>(id)}) {
        if (ast_.get_as_opt<function_expr>(expr->expression) || is_aggregate(expr->expression)) {
            return true;
        }
    }
    if (const auto us{ast_.get_as_opt<using_stmt>(id)}) {
        if (is_aggregate(us->explicit_type)) { return true; }
    }
    return false;
}

auto formatter::format_member_cfg_group(const cfg_item_group<member_handle>& group)
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
        for (const auto& item : arm.items) { items.emplace_back(format(item)); }
        parts.emplace_back(doc_manager_.group(doc_manager_.concat({
            doc_manager_.text("{"),
            doc_manager_.nest(doc_manager_.concat(
                {doc_manager_.line(), doc_manager_.join(std::move(items), doc_manager_.line())})),
            doc_manager_.line(),
            doc_manager_.text("}"),
        })));
    }
    return doc_manager_.concat(std::move(parts));
}

auto formatter::format_members(std::vector<syntax::doc_id>&                      entries,
                               const member_list&                                members,
                               const std::vector<cfg_item_group<member_handle>>& cfg_groups)
    -> void {
    const auto flush_cfg{[&](usize position) -> void {
        for (const auto& group : cfg_groups) {
            if (group.position == position) {
                entries.emplace_back(format_member_cfg_group(group));
            }
        }
    }};

    flush_cfg(0);
    for (usize i{0}; i < members.size(); ++i) {
        const auto& member{members[i]};
        auto        leading{consume_leading_comments(ast_.location_of(member).line)};
        auto        member_doc{format(member)};
        auto        trailing{consume_trailing_comment(ast_.end_location_of(member).line)};
        if (trailing != doc_manager_.nil()) {
            member_doc = doc_manager_.concat({member_doc, trailing});
        }
        if (leading != doc_manager_.nil()) {
            member_doc = doc_manager_.concat({leading, member_doc});
        }
        entries.emplace_back(member_doc);
        flush_cfg(i + 1);
    }
}

auto formatter::format_struct(const struct_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    if (node.is_extern) { head.emplace_back(doc_manager_.text("extern ")); }
    if (node.is_packed) { head.emplace_back(doc_manager_.text("packed ")); }
    head.emplace_back(doc_manager_.text("struct "));

    const auto field_item{[&](const struct_expr::field& field) -> syntax::doc_id {
        std::vector<syntax::doc_id> parts;
        if (field.is_public()) { parts.emplace_back(doc_manager_.text("pub ")); }
        parts.emplace_back(format(field.name));
        parts.emplace_back(doc_manager_.text(": "));
        if (field.explicit_alignment) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("@alignas("),
                                                    format(*field.explicit_alignment),
                                                    doc_manager_.text(") ")}));
        }
        parts.emplace_back(format(field.explicit_type));
        if (field.default_value) {
            parts.emplace_back(doc_manager_.text(" = "));
            parts.emplace_back(format(*field.default_value));
        }
        return doc_manager_.concat(std::move(parts));
    }};

    std::vector<syntax::doc_id> entries;
    const auto                  flush_cfg_groups{[&](usize position) -> void {
        for (const auto& group : node.cfg_groups) {
            if (group.position != position) { continue; }
            entries.emplace_back(format_aggregate_cfg_group(group, field_item));
        }
    }};

    flush_cfg_groups(0);
    for (usize i{0}; i < node.fields.size(); ++i) {
        const auto& field{node.fields[i]};
        const auto& start_loc{ast_.location_of(field.name)};
        auto        leading{consume_leading_comments(start_loc.line)};

        const auto end_line{field.default_value
                                ? ast_.end_location_of(*field.default_value).line
                                : (field.explicit_alignment
                                       ? ast_.end_location_of(*field.explicit_alignment).line
                                       : ast_.end_location_of(field.explicit_type).line)};
        const auto more_follows{i + 1 < node.fields.size() || !node.members.empty() ||
                                !node.cfg_groups.empty()};
        auto       field_doc{field_item(field)};
        auto       trailing{consume_trailing_comment(end_line)};
        if (more_follows || trailing != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({field_doc, doc_manager_.text(",")});
        } else {
            field_doc = doc_manager_.concat(
                {field_doc, doc_manager_.if_break(doc_manager_.text(","), doc_manager_.nil())});
        }
        if (trailing != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({field_doc, trailing});
        }
        if (leading != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({leading, field_doc});
        }
        entries.emplace_back(field_doc);
        flush_cfg_groups(i + 1);
    }

    const auto field_count{entries.size()};
    format_members(entries, node.members, node.member_cfg_groups);

    head.emplace_back(aggregate_body(std::move(entries), field_count));
    return doc_manager_.concat(std::move(head));
}

auto formatter::format_union(const union_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    if (node.is_extern) { head.emplace_back(doc_manager_.text("extern ")); }
    head.emplace_back(doc_manager_.text("union "));

    const auto field_item{[&](const union_expr::field& field) -> syntax::doc_id {
        std::vector<syntax::doc_id> parts;
        parts.emplace_back(format(field.name));
        parts.emplace_back(doc_manager_.text(": "));
        if (field.explicit_alignment) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("@alignas("),
                                                    format(*field.explicit_alignment),
                                                    doc_manager_.text(") ")}));
        }
        parts.emplace_back(format(field.explicit_type));
        return doc_manager_.concat(std::move(parts));
    }};

    std::vector<syntax::doc_id> entries;
    const auto                  flush_cfg_groups{[&](usize position) -> void {
        for (const auto& group : node.cfg_groups) {
            if (group.position == position) {
                entries.emplace_back(format_aggregate_cfg_group(group, field_item));
            }
        }
    }};

    flush_cfg_groups(0);
    for (usize i{0}; i < node.fields.size(); ++i) {
        const auto& [name, explicit_type, explicit_alignment]{node.fields[i]};
        const auto& start_loc{ast_.location_of(name)};
        auto        leading{consume_leading_comments(start_loc.line)};

        const auto end_line{explicit_alignment ? ast_.end_location_of(*explicit_alignment).line
                                               : ast_.end_location_of(explicit_type).line};
        const auto more_follows{i + 1 < node.fields.size() || !node.members.empty() ||
                                !node.cfg_groups.empty()};
        auto       field_doc{field_item(node.fields[i])};
        auto       trailing{consume_trailing_comment(end_line)};
        if (more_follows || trailing != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({field_doc, doc_manager_.text(",")});
        } else {
            field_doc = doc_manager_.concat(
                {field_doc, doc_manager_.if_break(doc_manager_.text(","), doc_manager_.nil())});
        }
        if (trailing != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({field_doc, trailing});
        }
        if (leading != doc_manager_.nil()) {
            field_doc = doc_manager_.concat({leading, field_doc});
        }
        entries.emplace_back(field_doc);
        flush_cfg_groups(i + 1);
    }
    const auto field_count{entries.size()};
    format_members(entries, node.members, node.member_cfg_groups);

    head.emplace_back(aggregate_body(std::move(entries), field_count));
    return doc_manager_.concat(std::move(head));
}

auto formatter::format_enum(const enum_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    head.emplace_back(doc_manager_.text("enum "));

    const auto variant_item{[&](const enum_expr::enumeration& variant) -> syntax::doc_id {
        return variant.value
                   ? doc_manager_.concat(
                         {format(variant.name), doc_manager_.text(" = "), format(*variant.value)})
                   : format(variant.name);
    }};

    std::vector<syntax::doc_id> entries;
    const auto                  flush_cfg_groups{[&](usize position) -> void {
        for (const auto& group : node.cfg_groups) {
            if (group.position == position) {
                entries.emplace_back(format_aggregate_cfg_group(group, variant_item));
            }
        }
    }};

    const auto total_enums{node.enumerations.size() + (node.non_exhaustive ? 1 : 0)};
    flush_cfg_groups(0);
    for (usize i{0}; i < node.enumerations.size(); ++i) {
        const auto& enumeration{node.enumerations[i]};
        const auto& start_loc{ast_.location_of(enumeration.name)};
        auto        leading{consume_leading_comments(start_loc.line)};

        auto       enum_doc{variant_item(enumeration)};
        const auto end_line{enumeration.value ? ast_.end_location_of(*enumeration.value).line
                                              : ast_.end_location_of(enumeration.name).line};
        const auto more_follows{i + 1 < total_enums || !node.members.empty() ||
                                !node.cfg_groups.empty()};
        auto       trailing{consume_trailing_comment(end_line)};
        if (more_follows || trailing != doc_manager_.nil()) {
            enum_doc = doc_manager_.concat({enum_doc, doc_manager_.text(",")});
        } else {
            enum_doc = doc_manager_.concat(
                {enum_doc, doc_manager_.if_break(doc_manager_.text(","), doc_manager_.nil())});
        }
        if (trailing != doc_manager_.nil()) {
            enum_doc = doc_manager_.concat({enum_doc, trailing});
        }
        if (leading != doc_manager_.nil()) { enum_doc = doc_manager_.concat({leading, enum_doc}); }
        entries.emplace_back(enum_doc);
        flush_cfg_groups(i + 1);
    }
    if (node.non_exhaustive) {
        auto non_ex{doc_manager_.text("_")};
        if (!node.members.empty()) {
            non_ex = doc_manager_.concat({non_ex, doc_manager_.text(",")});
        } else {
            non_ex = doc_manager_.concat(
                {non_ex, doc_manager_.if_break(doc_manager_.text(","), doc_manager_.nil())});
        }
        entries.emplace_back(non_ex);
    }
    const auto value_count{entries.size()};
    format_members(entries, node.members, node.member_cfg_groups);

    if (node.underlying) {
        head.emplace_back(doc_manager_.text(": "));
        head.emplace_back(format(*node.underlying));
        head.emplace_back(doc_manager_.text(" "));
    }
    head.emplace_back(aggregate_body(std::move(entries), value_count));
    return doc_manager_.concat(std::move(head));
}

auto formatter::aggregate_body(std::vector<syntax::doc_id> entries, usize comma_count)
    -> syntax::doc_id {
    if (entries.empty()) { return doc_manager_.text("{}"); }

    std::vector<syntax::doc_id> body;
    for (usize i{0}; i < entries.size(); ++i) {
        if (i != 0) {
            body.emplace_back(doc_manager_.line());
            if (i == comma_count) { body.emplace_back(doc_manager_.hard_line()); }
        }
        body.emplace_back(entries[i]);
    }

    return doc_manager_.group(doc_manager_.concat({
        doc_manager_.text("{"),
        doc_manager_.nest(
            doc_manager_.concat({doc_manager_.line(), doc_manager_.concat(std::move(body))})),
        doc_manager_.line(),
        doc_manager_.text("}"),
    }));
}

auto formatter::decl_prefix(const decl_stmt& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> parts;
    if (node.has_modifier(decl_modifiers::PUBLIC)) {
        parts.emplace_back(doc_manager_.text("pub "));
    }
    if (node.has_modifier(decl_modifiers::EXPORT)) {
        if (node.link_name) {
            parts.emplace_back(doc_manager_.concat(
                {doc_manager_.text("export("), format(*node.link_name), doc_manager_.text(") ")}));
        } else {
            parts.emplace_back(doc_manager_.text("export "));
        }
    }
    if (node.has_modifier(decl_modifiers::EXTERN)) {
        if (node.extern_target && node.link_name) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("extern("),
                                                    format(*node.extern_target),
                                                    doc_manager_.text(", "),
                                                    format(*node.link_name),
                                                    doc_manager_.text(") ")}));
        } else if (node.extern_target) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("extern("),
                                                    format(*node.extern_target),
                                                    doc_manager_.text(") ")}));
        } else {
            parts.emplace_back(doc_manager_.text("extern "));
        }
    }
    if (node.has_modifier(decl_modifiers::WEAK)) { parts.emplace_back(doc_manager_.text("weak ")); }
    if (node.has_modifier(decl_modifiers::THREADLOCAL)) {
        parts.emplace_back(doc_manager_.text("threadlocal "));
    }
    if (node.has_modifier(decl_modifiers::CONSTEXPR)) {
        parts.emplace_back(doc_manager_.text("constexpr "));
    } else if (node.has_modifier(decl_modifiers::CONSTANT)) {
        parts.emplace_back(doc_manager_.text("const "));
    } else if (node.has_modifier(decl_modifiers::VARIABLE)) {
        parts.emplace_back(doc_manager_.text("var "));
    }
    return doc_manager_.concat(std::move(parts));
}

auto formatter::tail_clause(node_id stmt) -> syntax::doc_id {
    if (const auto expr{ast_.get_as_opt<expr_stmt>(stmt)}) { return format(expr->expression); }
    if (const auto ret{ast_.get_as_opt<return_stmt>(stmt)}) {
        return doc_manager_.concat({
            doc_manager_.text("return"),
            ret->expression
                ? doc_manager_.concat({doc_manager_.text(" "), format(*ret->expression)})
                : doc_manager_.nil(),
        });
    }

    if (const auto brk{ast_.get_as_opt<break_stmt>(stmt)}) {
        return doc_manager_.concat({
            doc_manager_.text("break"),
            brk->label ? doc_manager_.concat({doc_manager_.text(" :"), format(*brk->label)})
                       : doc_manager_.nil(),
            brk->expression
                ? doc_manager_.concat({doc_manager_.text(" "), format(*brk->expression)})
                : doc_manager_.nil(),
        });
    }

    if (const auto cont{ast_.get_as_opt<continue_stmt>(stmt)}) {
        return doc_manager_.concat({
            doc_manager_.text("continue"),
            cont->label ? doc_manager_.concat({doc_manager_.text(" :"), format(*cont->label)})
                        : doc_manager_.nil(),
        });
    }
    return format(stmt);
}

auto formatter::visit(node_id, const array_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    head.emplace_back(doc_manager_.text("["));
    if (node.size) {
        head.emplace_back(format(*node.size));
    } else if (!node.is_type_expr) {
        head.emplace_back(doc_manager_.text("_"));
    }
    if (node.null_terminated) { head.emplace_back(doc_manager_.text(":0")); }
    head.emplace_back(doc_manager_.text("]"));
    if (node.mut_elements) { head.emplace_back(doc_manager_.text("mut ")); }
    head.emplace_back(format(node.item_explicit_type));
    if (node.is_type_expr) { return doc_manager_.concat(std::move(head)); }

    std::vector<syntax::doc_id> items;
    items.reserve(node.items.size());
    for (const auto& item : node.items) { items.emplace_back(format(item)); }
    head.emplace_back(doc_manager_.delimited("{", "}", std::move(items), true, true));

    return doc_manager_.concat(std::move(head));
}

auto formatter::visit(node_id, const asm_expr& node) -> syntax::doc_id {
    const auto operand_doc = [&](const asm_expr::operand& op) -> syntax::doc_id {
        return doc_manager_.concat({
            format(op.constraint),
            doc_manager_.text(" = "),
            op.value ? format(*op.value) : doc_manager_.text("_"),
        });
    };
    const auto operand_list = [&](std::string_view                      label,
                                  const std::vector<asm_expr::operand>& ops) -> syntax::doc_id {
        std::vector<syntax::doc_id> items;
        items.reserve(ops.size());
        for (const auto& op : ops) { items.emplace_back(operand_doc(op)); }
        return doc_manager_.concat({
            doc_manager_.text(label),
            doc_manager_.text(": "),
            doc_manager_.delimited("(", ")", std::move(items), false, false),
        });
    };

    std::vector<syntax::doc_id> clauses;
    clauses.emplace_back(doc_manager_.concat({
        doc_manager_.text("template: "),
        format(node.tmpl),
    }));
    if (!node.outputs.empty()) { clauses.emplace_back(operand_list("outputs", node.outputs)); }
    if (!node.inputs.empty()) { clauses.emplace_back(operand_list("inputs", node.inputs)); }
    if (!node.clobbers.empty()) {
        std::vector<syntax::doc_id> items;
        items.reserve(node.clobbers.size());
        for (const auto& clobber : node.clobbers) { items.emplace_back(format(clobber)); }
        clauses.emplace_back(doc_manager_.concat({
            doc_manager_.text("clobbers: "),
            doc_manager_.delimited("(", ")", std::move(items), false, false),
        }));
    }
    if (!node.options.empty()) {
        std::vector<syntax::doc_id> items;
        items.reserve(node.options.size());
        for (const auto opt : node.options) {
            items.emplace_back(doc_manager_.text(asm_option_spelling(opt)));
        }
        clauses.emplace_back(doc_manager_.concat({
            doc_manager_.text("options: "),
            doc_manager_.delimited("(", ")", std::move(items), false, false),
        }));
    }

    std::vector<syntax::doc_id> head;
    head.emplace_back(doc_manager_.text("asm "));
    if (node.result_type) {
        head.emplace_back(format(*node.result_type));
        head.emplace_back(doc_manager_.text(" "));
    }
    head.emplace_back(doc_manager_.delimited("{", "}", std::move(clauses), true, true));
    return doc_manager_.concat(std::move(head));
}

auto formatter::visit(node_id, const call_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> args;
    args.reserve(node.arguments.size());
    for (const auto& arg : node.arguments) {
        args.emplace_back(arg.visit([&](expr_handle h) { return format(h); },
                                    [&](explicit_type_id t) { return format(t); }));
    }
    return doc_manager_.concat({
        format(node.function),
        doc_manager_.delimited("(", ")", std::move(args), false, true),
    });
}

auto formatter::visit(node_id, const do_while_loop_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("do "),
        format(node.block),
        doc_manager_.text(" while ("),
        format(node.condition),
        doc_manager_.text(")"),
    });
}

auto formatter::visit(node_id, const enum_expr& node) -> syntax::doc_id {
    return format_enum(node);
}

auto formatter::visit(node_id, const for_loop_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> iterables;
    iterables.reserve(node.iterables.size());
    for (const auto& it : node.iterables) { iterables.emplace_back(format(it)); }

    std::vector<syntax::doc_id> captures;
    captures.reserve(node.captures.size());
    for (const auto& capture : node.captures) {
        captures.emplace_back(doc_manager_.concat(
            {doc_manager_.text(modifier_prefix(capture.modifier)), format(capture.payload)}));
    }

    return doc_manager_.concat({
        doc_manager_.text("for "),
        doc_manager_.delimited("(", ")", std::move(iterables), false, false),
        doc_manager_.text(" |"),
        doc_manager_.join(std::move(captures), doc_manager_.text(", ")),
        doc_manager_.text("| "),
        format(node.block),
        node.non_break
            ? doc_manager_.concat({doc_manager_.text(" else "), tail_clause(*node.non_break)})
            : doc_manager_.nil(),
    });
}

auto formatter::visit(node_id, const function_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> params;
    if (node.self) {
        params.emplace_back(doc_manager_.concat(
            {doc_manager_.text(modifier_prefix(node.self->modifier)), format(node.self->name)}));
    }
    for (const auto& param : node.parameters) {
        params.emplace_back(
            doc_manager_.concat({doc_manager_.text(param.is_constexpr ? "constexpr " : ""),
                                 format(param.name),
                                 doc_manager_.text(": "),
                                 format(param.explicit_type)}));
    }
    if (node.variadic) { params.emplace_back(doc_manager_.text("...")); }

    const auto callconv_doc{node.conv == calling_convention::C
                                ? doc_manager_.nil()
                                : doc_manager_.owned(fmt::format(
                                      " callconv(.{})", calling_convention_name(node.conv)))};

    if (node.is_type_expr) {
        return doc_manager_.concat({
            doc_manager_.text("fn"),
            doc_manager_.delimited("(", ")", std::move(params), false, true),
            callconv_doc,
            doc_manager_.text(": "),
            format(node.explicit_return_type),
        });
    }

    return doc_manager_.concat({
        node.is_move ? doc_manager_.text("move ") : doc_manager_.nil(),
        node.is_naked ? doc_manager_.text("naked ") : doc_manager_.nil(),
        doc_manager_.text("fn"),
        doc_manager_.delimited("(", ")", std::move(params), false, true),
        callconv_doc,
        doc_manager_.text(": "),
        format(node.explicit_return_type),
        doc_manager_.text(" "),
        format(node.body),
    });
}

auto formatter::visit(node_id id, const identifier_expr& node) -> syntax::doc_id {
    if (syntax::get_builtin_opt(id.get_token_type()) && !node.name.starts_with('@')) {
        return doc_manager_.owned(fmt::format("@{}", node.name));
    }
    return doc_manager_.text(node.name);
}

auto formatter::visit(node_id, const if_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("if "),
        node.constexpr_condition ? doc_manager_.text("constexpr ") : doc_manager_.nil(),
        doc_manager_.text("("),
        format(node.condition),
        doc_manager_.text(") "),
        node.alternate ? format(node.consequence) : tail_clause(node.consequence),
        node.alternate
            ? doc_manager_.concat({doc_manager_.text(" else "), tail_clause(*node.alternate)})
            : doc_manager_.nil(),
    });
}

auto formatter::visit(node_id, const index_expr& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {format(node.array), doc_manager_.text("["), format(node.index), doc_manager_.text("]")});
}

auto formatter::visit(node_id, const infinite_loop_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({doc_manager_.text("loop "), format(node.block)});
}

auto formatter::visit(node_id id, const assignment_expr& node) -> syntax::doc_id {
    return doc_manager_.group(doc_manager_.concat({
        format(node.lhs),
        doc_manager_.text(" "),
        doc_manager_.text(operator_spelling(id.get_token_type())),
        doc_manager_.nest(doc_manager_.concat({doc_manager_.line(), format(node.rhs)})),
    }));
}

auto formatter::visit(node_id id, const binary_expr& node) -> syntax::doc_id {
    return doc_manager_.group(doc_manager_.concat({
        format(node.lhs),
        doc_manager_.nest(doc_manager_.concat({
            doc_manager_.text(" "),
            doc_manager_.text(operator_spelling(id.get_token_type())),
            doc_manager_.line(),
            format(node.rhs),
        })),
    }));
}

auto formatter::visit(node_id, const dot_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({format(node.object), doc_manager_.text("."), format(node.member)});
}

auto formatter::visit(node_id id, const range_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({format(node.lhs),
                                doc_manager_.text(operator_spelling(id.get_token_type())),
                                format(node.rhs)});
}

auto formatter::visit(node_id, const initializer_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> parts;
    parts.emplace_back(node.object_type ? format(*node.object_type) : doc_manager_.text("."));

    std::vector<syntax::doc_id> inits;
    inits.reserve(node.initializers.size());
    for (const auto& init : node.initializers) {
        if (init.member) {
            inits.emplace_back(doc_manager_.concat(
                {format(*init.member), doc_manager_.text(" = "), format(init.value)}));
        } else {
            inits.emplace_back(format(init.value));
        }
    }
    parts.emplace_back(doc_manager_.delimited("{", "}", std::move(inits), true, true));

    return doc_manager_.concat(std::move(parts));
}

auto formatter::visit(node_id, const label_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({format(node.name), doc_manager_.text(": "), format(node.body)});
}

auto formatter::visit(node_id id, const match_expr& node) -> syntax::doc_id {
    const auto& match_end{ast_.end_location_of(id)};

    std::vector<syntax::doc_id> arms;
    for (const auto& arm : node.arms) {
        const auto& start_loc{ast_.location_of(arm.pattern)};
        auto        leading{consume_leading_comments(start_loc.line)};

        std::vector<syntax::doc_id> parts;
        parts.emplace_back(format(arm.pattern));
        parts.emplace_back(doc_manager_.text(" => "));
        if (arm.capture) {
            parts.emplace_back(doc_manager_.text("|"));
            parts.emplace_back(doc_manager_.text(modifier_prefix(arm.modifier)));
            parts.emplace_back(format(*arm.capture));
            parts.emplace_back(doc_manager_.text("| "));
        }
        parts.emplace_back(format(arm.dispatch));

        const auto end_line{ast_.end_location_of(arm.dispatch).line};
        auto       arm_doc{doc_manager_.concat(std::move(parts))};
        auto       trailing{consume_trailing_comment(end_line)};
        if (trailing != doc_manager_.nil()) { arm_doc = doc_manager_.concat({arm_doc, trailing}); }
        if (leading != doc_manager_.nil()) { arm_doc = doc_manager_.concat({leading, arm_doc}); }
        arms.emplace_back(arm_doc);
    }

    auto dangling{consume_dangling_comments(match_end.line)};
    if (dangling != doc_manager_.nil()) { arms.emplace_back(dangling); }

    return doc_manager_.concat({
        doc_manager_.text("match ("),
        format(node.matcher),
        doc_manager_.text(") "),
        doc_manager_.delimited("{", "}", std::move(arms), true, true),
    });
}

auto formatter::visit(node_id id, const reference_expr& node) -> syntax::doc_id {
    const auto op{operator_spelling(id.get_token_type())};
    return doc_manager_.concat({doc_manager_.text(op),
                                prefix_needs_gap(op) ? doc_manager_.text(" ") : doc_manager_.nil(),
                                format(node.rhs)});
}

auto formatter::visit(node_id id, const address_of_expr& node) -> syntax::doc_id {
    const auto op{operator_spelling(id.get_token_type())};
    return doc_manager_.concat({doc_manager_.text(op),
                                prefix_needs_gap(op) ? doc_manager_.text(" ") : doc_manager_.nil(),
                                format(node.rhs)});
}

auto formatter::visit(node_id id, const dereference_expr& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {doc_manager_.text(operator_spelling(id.get_token_type())), format(node.rhs)});
}

auto formatter::visit(node_id id, const unary_expr& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {doc_manager_.text(operator_spelling(id.get_token_type())), format(node.rhs)});
}

auto formatter::visit(node_id id, const unwrap_expr& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {format(node.operand), doc_manager_.text(operator_spelling(id.get_token_type()))});
}

auto formatter::visit(node_id, const implicit_access_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({doc_manager_.text("."), format(node.member)});
}

#define MAKE_VERBATIM_FORMAT(Type, txt)                                                   \
    auto formatter::visit([[maybe_unused]] node_id id, [[maybe_unused]] const Type& node) \
        -> syntax::doc_id {                                                               \
        return doc_manager_.text(txt);                                                    \
    }

MAKE_VERBATIM_FORMAT(string_expr, node.spelling)
MAKE_VERBATIM_FORMAT(i32_expr, node.spelling)
MAKE_VERBATIM_FORMAT(i64_expr, node.spelling)
MAKE_VERBATIM_FORMAT(isize_expr, node.spelling)
MAKE_VERBATIM_FORMAT(u32_expr, node.spelling)
MAKE_VERBATIM_FORMAT(u64_expr, node.spelling)
MAKE_VERBATIM_FORMAT(usize_expr, node.spelling)
MAKE_VERBATIM_FORMAT(u8_expr, node.spelling)
MAKE_VERBATIM_FORMAT(f32_expr, node.spelling)
MAKE_VERBATIM_FORMAT(f64_expr, node.spelling)
MAKE_VERBATIM_FORMAT(bool_expr,
                     id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE ? "true" : "false")
MAKE_VERBATIM_FORMAT(void_expr, "{}")
MAKE_VERBATIM_FORMAT(undefined_expr, "undefined")
MAKE_VERBATIM_FORMAT(nullptr_expr, "nullptr")
MAKE_VERBATIM_FORMAT(unreachable_expr, "unreachable")

auto formatter::visit(node_id, const module_access_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({format(node.outer), doc_manager_.text("::"), format(node.inner)});
}

auto formatter::visit(node_id, const struct_expr& node) -> syntax::doc_id {
    return format_struct(node);
}

auto formatter::visit(node_id, const union_expr& node) -> syntax::doc_id {
    return format_union(node);
}

auto formatter::visit(node_id, const while_loop_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("while ("),
        format(node.condition),
        doc_manager_.text(")"),
        node.continuation
            ? doc_manager_.concat(
                  {doc_manager_.text(" : ("), format(*node.continuation), doc_manager_.text(")")})
            : doc_manager_.nil(),
        doc_manager_.text(" "),
        format(node.block),
        node.non_break
            ? doc_manager_.concat({doc_manager_.text(" else "), tail_clause(*node.non_break)})
            : doc_manager_.nil(),
    });
}

auto formatter::visit(node_id, const cfg_value_expr& node) -> syntax::doc_id {
    if (node.predicate) {
        return doc_manager_.concat({
            doc_manager_.text("@cfgValue("),
            format(*node.predicate),
            doc_manager_.text(")"),
        });
    }

    std::vector<syntax::doc_id> arms;
    arms.reserve(node.guards.size() + 1);
    for (const auto& guard : node.guards) {
        arms.emplace_back(doc_manager_.concat(
            {format(guard.predicate), doc_manager_.text(" => "), format(guard.value)}));
    }
    if (node.fallback) {
        arms.emplace_back(
            doc_manager_.concat({doc_manager_.text("_ => "), format(*node.fallback)}));
    }

    return doc_manager_.concat({
        doc_manager_.text("@cfgValue"),
        doc_manager_.delimited("(", ")", std::move(arms), true, true),
    });
}

auto formatter::visit(node_id, const cfg_stmt& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> parts;
    for (auto it{node.arms.begin()}; it != node.arms.end(); ++it) {
        if (it != node.arms.begin()) { parts.emplace_back(doc_manager_.text(" else ")); }
        if (it->predicate) {
            parts.emplace_back(doc_manager_.text("@cfg ("));
            parts.emplace_back(format(*it->predicate));
            parts.emplace_back(doc_manager_.text(") "));
        }

        std::vector<syntax::doc_id> body;
        for (const auto& item : it->items) {
            if (!body.empty()) { body.emplace_back(doc_manager_.hard_line()); }
            body.emplace_back(format(item));
        }
        parts.emplace_back(doc_manager_.concat({
            doc_manager_.text("{"),
            doc_manager_.nest(doc_manager_.concat(
                {doc_manager_.hard_line(), doc_manager_.concat(std::move(body))})),
            doc_manager_.hard_line(),
            doc_manager_.text("}"),
        }));
    }
    return doc_manager_.concat(std::move(parts));
}

auto formatter::visit(node_id id, const block_stmt& node) -> syntax::doc_id {
    const auto& block_start{ast_.location_of(id)};
    const auto& block_end{ast_.end_location_of(id)};

    auto header_trailing{consume_trailing_comment(block_start.line)};

    std::vector<syntax::doc_id> body;
    auto                        previous{node_id::make_invalid()};

    for (const auto& stmt : node.statements) {
        const auto& stmt_start{ast_.location_of(*stmt)};
        const auto& stmt_end{ast_.end_location_of(*stmt)};

        auto leading{consume_leading_comments(stmt_start.line)};

        if (!body.empty()) {
            body.emplace_back(doc_manager_.hard_line());
            if (leading == doc_manager_.nil() && blank_line_between(previous, *stmt)) {
                body.emplace_back(doc_manager_.hard_line());
            }
        }

        auto stmt_doc{format(stmt)};
        auto trailing{consume_trailing_comment(stmt_end.line)};
        if (trailing != doc_manager_.nil()) {
            stmt_doc = doc_manager_.concat({stmt_doc, trailing});
        }
        if (leading != doc_manager_.nil()) { stmt_doc = doc_manager_.concat({leading, stmt_doc}); }

        body.emplace_back(stmt_doc);
        previous = *stmt;
    }

    auto dangling{consume_dangling_comments(block_end.line)};
    if (dangling != doc_manager_.nil()) {
        if (!body.empty()) { body.emplace_back(doc_manager_.hard_line()); }
        body.emplace_back(dangling);
    }

    if (body.empty()) {
        if (header_trailing != doc_manager_.nil()) {
            return doc_manager_.concat(
                {doc_manager_.text("{"), header_trailing, doc_manager_.text("}")});
        }
        return doc_manager_.text("{}");
    }

    std::vector<syntax::doc_id> open_parts;
    open_parts.emplace_back(doc_manager_.text("{"));
    if (header_trailing != doc_manager_.nil()) { open_parts.emplace_back(header_trailing); }

    return doc_manager_.concat({
        doc_manager_.concat(std::move(open_parts)),
        doc_manager_.nest(
            doc_manager_.concat({doc_manager_.hard_line(), doc_manager_.concat(std::move(body))})),
        doc_manager_.hard_line(),
        doc_manager_.text("}"),
    });
}

auto formatter::visit(node_id, const break_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("break"),
        node.label ? doc_manager_.concat({doc_manager_.text(" :"), format(*node.label)})
                   : doc_manager_.nil(),
        node.expression ? doc_manager_.concat({doc_manager_.text(" "), format(*node.expression)})
                        : doc_manager_.nil(),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, const continue_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("continue"),
        node.label ? doc_manager_.concat({doc_manager_.text(" :"), format(*node.label)})
                   : doc_manager_.nil(),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, const decl_stmt& node) -> syntax::doc_id {
    const auto walrus{!node.explicit_type && node.value};
    return doc_manager_.concat({
        decl_prefix(node),
        format(node.name),
        node.explicit_type
            ? doc_manager_.concat({doc_manager_.text(": "), format(*node.explicit_type)})
            : doc_manager_.nil(),
        node.value
            ? doc_manager_.concat({doc_manager_.text(walrus ? " := " : " = "), format(*node.value)})
            : doc_manager_.nil(),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, const defer_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({doc_manager_.text("defer "), format(node.deferred)});
}

auto formatter::visit(node_id, const discard_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {doc_manager_.text("_ = "), format(node.discarded), doc_manager_.text(";")});
}

auto formatter::visit(node_id, const expr_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat(
        {format(node.expression), node.terminated ? doc_manager_.text(";") : doc_manager_.nil()});
}

auto formatter::visit(node_id id, const import_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        import_stmt::is_public(id) ? doc_manager_.text("pub ") : doc_manager_.nil(),
        doc_manager_.text("import "),
        format(node.payload),
        node.alias ? doc_manager_.concat({doc_manager_.text(" as "), format(*node.alias)})
                   : doc_manager_.nil(),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, const return_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("return"),
        node.expression ? doc_manager_.concat({doc_manager_.text(" "), format(*node.expression)})
                        : doc_manager_.nil(),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, const test_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        doc_manager_.text("test "),
        node.description ? doc_manager_.concat({format(*node.description), doc_manager_.text(" ")})
                         : doc_manager_.nil(),
        format(node.block),
    });
}

auto formatter::visit(node_id id, const using_stmt& node) -> syntax::doc_id {
    return doc_manager_.concat({
        using_stmt::is_public(id) ? doc_manager_.text("pub ") : doc_manager_.nil(),
        doc_manager_.text("using "),
        format(node.alias),
        doc_manager_.text(" = "),
        format(node.explicit_type),
        doc_manager_.text(";"),
    });
}

auto formatter::visit(node_id, stdx::monostate) -> syntax::doc_id { return doc_manager_.text("_"); }

auto formatter::visit(explicit_type_id id, const identifier_expr& node) -> syntax::doc_id {
    return with_modifier(id, doc_manager_.text(node.name));
}

auto formatter::visit(explicit_type_id id, const module_access_expr& node) -> syntax::doc_id {
    return with_modifier(
        id, doc_manager_.concat({format(node.outer), doc_manager_.text("::"), format(node.inner)}));
}

auto formatter::visit(explicit_type_id id, const dot_expr& node) -> syntax::doc_id {
    return with_modifier(
        id,
        doc_manager_.concat({format(node.object), doc_manager_.text("."), format(node.member)}));
}

auto formatter::visit(explicit_type_id id, const call_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> args;
    args.reserve(node.arguments.size());
    for (const auto& arg : node.arguments) {
        args.emplace_back(arg.visit([&](auto h) { return format(h); }));
    }
    return with_modifier(
        id,
        doc_manager_.concat({format(node.function),
                             doc_manager_.delimited("(", ")", std::move(args), false, false)}));
}

auto formatter::visit(explicit_type_id id, const explicit_function_type& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> params;
    params.reserve(node.parameter_types.size());
    for (const auto type : node.parameter_types) { params.emplace_back(format(type)); }
    if (node.variadic) { params.emplace_back(doc_manager_.text("...")); }

    return with_modifier(id,
                         doc_manager_.concat({
                             doc_manager_.text("fn"),
                             doc_manager_.delimited("(", ")", std::move(params), false, false),
                             doc_manager_.text(": "),
                             format(node.explicit_return_type),
                         }));
}

auto formatter::visit(explicit_type_id id, const explicit_type_id& node) -> syntax::doc_id {
    return with_modifier(id, format(node));
}

auto formatter::visit(explicit_type_id id, const struct_expr& node) -> syntax::doc_id {
    return with_modifier(id, format_struct(node));
}

auto formatter::visit(explicit_type_id id, const enum_expr& node) -> syntax::doc_id {
    return with_modifier(id, format_enum(node));
}

auto formatter::visit(explicit_type_id id, const union_expr& node) -> syntax::doc_id {
    return with_modifier(id, format_union(node));
}

auto formatter::visit(explicit_type_id id, const explicit_array_type& node) -> syntax::doc_id {
    return with_modifier(id,
                         doc_manager_.concat({
                             doc_manager_.text("["),
                             node.dimension ? format(*node.dimension) : doc_manager_.nil(),
                             node.null_terminated ? doc_manager_.text(":0") : doc_manager_.nil(),
                             doc_manager_.text("]"),
                             node.mut_elements ? doc_manager_.text("mut ") : doc_manager_.nil(),
                             format(node.inner_explicit_type),
                         }));
}

} // namespace ghoti::ast
