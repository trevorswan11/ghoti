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

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"
#include "compiler/syntax/operators.hh"
#include "compiler/syntax/token_type.hh"

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

[[nodiscard]] auto prefix_needs_gap(std::string_view op) -> bool {
    return !op.empty() && (std::isalnum(static_cast<u8>(op.back())) != 0);
}

} // namespace

auto formatter::format() -> void {
    auto previous{node_id::make_invalid()};
    for (const auto id : ast_) {
        if (previous.is_valid() && blank_line_between(previous, id)) {
            doc_manager_.add_root(doc_manager_.hard_line());
        }
        doc_manager_.add_root(format(id));
        doc_manager_.add_root(doc_manager_.hard_line());
        previous = id;
    }

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

auto formatter::format_struct(const struct_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    if (node.is_extern) { head.emplace_back(doc_manager_.text("extern ")); }
    if (node.is_packed) { head.emplace_back(doc_manager_.text("packed ")); }
    head.emplace_back(doc_manager_.text("struct "));

    std::vector<syntax::doc_id> entries;
    for (const auto& field : node.fields) {
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
        entries.emplace_back(doc_manager_.concat(std::move(parts)));
    }
    const auto field_count{entries.size()};
    for (const auto& member : node.members) { entries.emplace_back(format(member)); }

    head.emplace_back(aggregate_body(std::move(entries), field_count));
    return doc_manager_.concat(std::move(head));
}

auto formatter::format_union(const union_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    if (node.is_extern) { head.emplace_back(doc_manager_.text("extern ")); }
    head.emplace_back(doc_manager_.text("union "));

    std::vector<syntax::doc_id> entries;
    for (const auto& [name, explicit_type, explicit_alignment] : node.fields) {
        std::vector<syntax::doc_id> parts;
        parts.emplace_back(format(name));
        parts.emplace_back(doc_manager_.text(": "));
        if (explicit_alignment) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("@alignas("),
                                                    format(*explicit_alignment),
                                                    doc_manager_.text(") ")}));
        }
        parts.emplace_back(format(explicit_type));
        entries.emplace_back(doc_manager_.concat(std::move(parts)));
    }
    const auto field_count{entries.size()};
    for (const auto& member : node.members) { entries.emplace_back(format(member)); }

    head.emplace_back(aggregate_body(std::move(entries), field_count));
    return doc_manager_.concat(std::move(head));
}

auto formatter::format_enum(const enum_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> head;
    head.emplace_back(doc_manager_.text("enum "));

    std::vector<syntax::doc_id> entries;
    for (const auto& enumeration : node.enumerations) {
        entries.emplace_back(enumeration.value ? doc_manager_.concat({format(enumeration.name),
                                                                      doc_manager_.text(" = "),
                                                                      format(*enumeration.value)})
                                               : format(enumeration.name));
    }
    if (node.non_exhaustive) { entries.emplace_back(doc_manager_.text("_")); }
    const auto value_count{entries.size()};
    for (const auto& member : node.members) { entries.emplace_back(format(member)); }

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
        const auto is_field{i < comma_count};
        const auto more_follows{i + 1 < comma_count || comma_count < entries.size()};
        if (is_field && more_follows) { body.emplace_back(doc_manager_.text(",")); }
    }
    const auto trailing{comma_count != 0 && comma_count == entries.size()
                            ? doc_manager_.if_break(doc_manager_.text(","), doc_manager_.nil())
                            : doc_manager_.nil()};

    return doc_manager_.group(doc_manager_.concat({
        doc_manager_.text("{"),
        doc_manager_.nest(doc_manager_.concat(
            {doc_manager_.line(), doc_manager_.concat(std::move(body)), trailing})),
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
        parts.emplace_back(doc_manager_.text("export "));
    }
    if (node.has_modifier(decl_modifiers::EXTERN)) {
        if (node.extern_target) {
            parts.emplace_back(doc_manager_.concat({doc_manager_.text("extern("),
                                                    format(*node.extern_target),
                                                    doc_manager_.text(") ")}));
        } else {
            parts.emplace_back(doc_manager_.text("extern "));
        }
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
    head.emplace_back(node.size ? format(*node.size) : doc_manager_.text("_"));
    if (node.null_terminated) { head.emplace_back(doc_manager_.text(":0")); }
    head.emplace_back(doc_manager_.text("]"));
    if (node.mut_elements) { head.emplace_back(doc_manager_.text("mut ")); }
    head.emplace_back(format(node.item_explicit_type));

    std::vector<syntax::doc_id> items;
    items.reserve(node.items.size());
    for (const auto& item : node.items) { items.emplace_back(format(item)); }
    head.emplace_back(doc_manager_.delimited("{", "}", std::move(items), true, true));

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
        params.emplace_back(doc_manager_.concat(
            {format(param.name), doc_manager_.text(": "), format(param.explicit_type)}));
    }
    if (node.variadic) { params.emplace_back(doc_manager_.text("...")); }

    return doc_manager_.concat({
        node.is_move ? doc_manager_.text("move ") : doc_manager_.nil(),
        doc_manager_.text("fn"),
        doc_manager_.delimited("(", ")", std::move(params), false, true),
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
        inits.emplace_back(doc_manager_.concat(
            {format(init.member), doc_manager_.text(" = "), format(init.value)}));
    }
    parts.emplace_back(doc_manager_.delimited("{", "}", std::move(inits), true, true));

    return doc_manager_.concat(std::move(parts));
}

auto formatter::visit(node_id, const label_expr& node) -> syntax::doc_id {
    return doc_manager_.concat({format(node.name), doc_manager_.text(": "), format(node.body)});
}

auto formatter::visit(node_id, const match_expr& node) -> syntax::doc_id {
    std::vector<syntax::doc_id> arms;
    arms.reserve(node.arms.size());
    for (const auto& arm : node.arms) {
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
        arms.emplace_back(doc_manager_.concat(std::move(parts)));
    }
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

auto formatter::visit(node_id, const block_stmt& node) -> syntax::doc_id {
    if (node.statements.empty()) { return doc_manager_.text("{}"); }

    std::vector<syntax::doc_id> body;
    auto                        previous{node_id::make_invalid()};
    for (const auto& stmt : node.statements) {
        if (!body.empty()) {
            body.emplace_back(doc_manager_.hard_line());
            if (blank_line_between(previous, *stmt)) {
                body.emplace_back(doc_manager_.hard_line());
            }
        }
        body.emplace_back(format(stmt));
        previous = *stmt;
    }

    return doc_manager_.concat({
        doc_manager_.text("{"),
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
