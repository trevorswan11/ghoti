#include "compiler/ast/dumper.hh"

#include <sstream>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_flags.hpp>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token_type.hh"
#include "support/indent.hh"

namespace ghoti::ast {

auto dumper::compare_source_asts(std::string_view s1, std::string_view s2) -> bool {
    syntax::parser p1{s1}, p2{s2};
    ast::AST       s1_ast, s2_ast;
    const auto     diag1{p1.consume(s1_ast)}, diag2{p2.consume(s2_ast)};
    if (!diag1.empty() || !diag2.empty()) { return false; }

    std::ostringstream s1_oss, s2_oss;
    ast::dumper        dumper1{s1_ast, s1_oss}, dumper2{s2_ast, s2_oss};
    dumper1.dump();
    dumper2.dump();
    if (s1_oss.view() != s2_oss.view()) { return false; }
    return true;
}

auto dumper::visit(node_id, const array_expr& array) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ArrayExpression{}", array.is_type_expr ? " (type)" : "");
    {
        const indent::guard g{indent_, false};
        if (array.size) {
            fmt::print(out_, "{}Size: ", indent_.current_branch());
            dump(*array.size);
        } else {
            fmt::println(out_, "{}Size: (inferred)", indent_.current_branch());
        }
    }

    {
        const indent::guard g_inner{indent_, false};
        fmt::println(
            out_, "{}Null terminated: {}", indent_.current_branch(), array.null_terminated);
    }

    {
        const indent::guard g{indent_, array.is_type_expr};
        fmt::print(out_, "{}Type: ", indent_.current_branch());
        dump(array.item_explicit_type);
    }
    if (array.is_type_expr) { return; }

    const indent::guard g{indent_, true};
    fmt::print(out_, "{}Items:", indent_.current_branch());
    if (array.items.empty()) {
        fmt::println(out_, " <empty>");
    } else {
        fmt::println(out_, "");
        dump_node_list(array.items);
    }
}

auto dumper::visit(node_id, const asm_expr& asm_node) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "AsmExpression");

    const auto dump_operands = [this](std::string_view                   label,
                                      gsl::span<const asm_expr::operand> operands,
                                      bool                               last) -> void {
        const indent::guard g{indent_, last};
        if (operands.empty()) {
            fmt::println(out_, "{}{}: <empty>", indent_.current_branch(), label);
            return;
        }
        fmt::println(out_, "{}{}:", indent_.current_branch(), label);
        dump_container(operands, [this](const asm_expr::operand& op) -> void {
            fmt::print(out_, "{}", indent_.current_branch());
            dump(op.constraint);
            const indent::guard g_inner{indent_, true};
            fmt::print(out_, "{}Value: ", indent_.current_branch());
            if (op.value) {
                dump(*op.value);
            } else {
                fmt::println(out_, "_ (result slot)");
            }
        });
    };

    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Template: ", indent_.current_branch());
        dump(asm_node.tmpl);
    }

    {
        const indent::guard g{indent_, false};
        if (asm_node.result_type) {
            fmt::print(out_, "{}ResultType: ", indent_.current_branch());
            dump(*asm_node.result_type);
        } else {
            fmt::println(out_, "{}ResultType: (none)", indent_.current_branch());
        }
    }

    dump_operands("Outputs", asm_node.outputs, false);
    dump_operands("Inputs", asm_node.inputs, false);

    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Clobbers:", indent_.current_branch());
        if (asm_node.clobbers.empty()) {
            fmt::println(out_, " <empty>");
        } else {
            fmt::println(out_, "");
            dump_container(asm_node.clobbers, [this](auto clobber) -> void {
                fmt::print(out_, "{}", indent_.current_branch());
                dump(clobber);
            });
        }
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Options:", indent_.current_branch());
        if (asm_node.options.empty()) {
            fmt::println(out_, " <none>");
        } else {
            for (const auto opt : asm_node.options) {
                fmt::print(out_, " {}", magic_enum::enum_name(opt));
            }
            fmt::println(out_, "");
        }
    }
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const call_expr& call) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "CallExpression");
    const auto has_args{!call.arguments.empty()};
    {
        const indent::guard g{indent_, !has_args};
        fmt::print(out_, "{}Callee: ", indent_.current_branch());
        dump(call.function);
    }

    if (has_args) {
        const indent::guard g{indent_, true};
        fmt::println(out_, "{}Arguments:", indent_.current_branch());
        dump_container(call.arguments, [this](const call_expr::argument& arg) -> void {
            fmt::print(out_, "{}", indent_.current_branch());
            arg.visit([this](auto arg_id) -> void { dump(arg_id); });
        });
    }
}

auto dumper::visit(node_id, const do_while_loop_expr& do_while) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "DoWhileLoopExpression");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Body: ", indent_.current_branch());
        dump(do_while.block);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Condition: ", indent_.current_branch());
        dump(do_while.condition);
    }
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const enum_expr& enum_expr) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "EnumExpression");

    if (enum_expr.underlying) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Underlying: ", indent_.current_branch());
        dump(*enum_expr.underlying);
    }

    if (!enum_expr.enumerations.empty()) {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Enumerations:", indent_.current_branch());
        dump_container(enum_expr.enumerations,
                       [this](const enum_expr::enumeration& enumeration) -> void {
                           {
                               fmt::print(out_, "{}Name: ", indent_.current_branch());
                               const indent::guard g_name{indent_, !enumeration.value};
                               dump(enumeration.name);
                           }

                           if (enumeration.value) {
                               const indent::guard g_val{indent_, true};
                               fmt::print(out_, "{}Default: ", indent_.current_branch());
                               dump(*enumeration.value);
                           }
                       });
    }

    if (!enum_expr.cfg_groups.empty()) {
        const indent::guard g{indent_, false};
        dump_cfg_groups(enum_expr.cfg_groups);
    }

    const auto has_members{!enum_expr.members.empty()};
    const auto has_member_cfg{!enum_expr.member_cfg_groups.empty()};
    {
        const indent::guard g{indent_, !has_members && !has_member_cfg};
        fmt::println(
            out_, "{}Non-Exhaustive: {}", indent_.current_branch(), enum_expr.non_exhaustive);
    }

    if (has_members) {
        const indent::guard g{indent_, !has_member_cfg};
        fmt::println(out_, "{}Members:", indent_.current_branch());
        dump_node_list(enum_expr.members);
    }

    if (has_member_cfg) {
        const indent::guard g{indent_, true};
        dump_cfg_groups(enum_expr.member_cfg_groups);
    }
}

auto dumper::visit(node_id, const for_loop_expr& for_loop) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ForLoopExpression");
    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Iterables:", indent_.current_branch());
        dump_node_list(for_loop.iterables);
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Captures:", indent_.current_branch());
        dump_container(for_loop.captures, [this](const for_loop_expr::capture& capture) -> void {
            fmt::print(out_, "{}", indent_.current_branch());
            if (capture.payload.is<ast::discarded>()) {
                fmt::println(out_, "<discarded>");
            } else {
                const auto& ident{ast_.get_as<identifier_expr>(*capture.payload)};
                fmt::println(out_, "{} (modifier: {})", ident, capture.modifier);
            }
        });
    }

    const auto has_non_break{for_loop.non_break.has_value()};
    {
        const indent::guard g{indent_, !has_non_break};
        fmt::print(out_, "{}Body: ", indent_.current_branch());
        dump(for_loop.block);
    }

    if (has_non_break) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Non-Break: ", indent_.current_branch());
        dump(*for_loop.non_break);
    }
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const function_expr& function) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "FunctionExpression");
    if (function.self) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}", indent_.current_branch());
        const auto& ident{ast_.get_as<identifier_expr>(*function.self->name)};
        fmt::println(out_, "Self: {} (modifier: {})", ident, function.self->modifier);
    }

    if (!function.parameters.empty()) {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Parameters:", indent_.current_branch());
        dump_container(function.parameters,
                       [this](const function_expr::parameter& parameter) -> void {
                           fmt::println(out_,
                                        "{}Param{}:",
                                        indent_.current_branch(),
                                        parameter.is_constexpr ? " (constexpr)" : "");
                           {
                               const indent::guard g_name{indent_, false};
                               fmt::print(out_, "{}Name: ", indent_.current_branch());
                               dump(*parameter.name);
                           }

                           {
                               const indent::guard g_type{indent_, true};
                               fmt::print(out_, "{}Type: ", indent_.current_branch());
                               dump(parameter.explicit_type);
                           }
                       });
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Variadic: {}", indent_.current_branch(), function.variadic);
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Move: {}", indent_.current_branch(), function.is_move);
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Naked: {}", indent_.current_branch(), function.is_naked);
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_,
                     "{}CallConv: {}",
                     indent_.current_branch(),
                     calling_convention_name(function.conv));
    }

    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Returns: ", indent_.current_branch());
        dump(function.explicit_return_type);
    }

    if (!function.is_type_expr) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Body: ", indent_.current_branch());
        dump(*function.body);
    }
}

auto dumper::visit(node_id id, const identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    fmt::print(out_, "IdentifierExpression: {}", ident);
    if (syntax::get_builtin_opt(id.get_token_type())) {
        fmt::print(out_, " (builtin)");
    } else if (syntax::token_type::is_primitive(id.get_token_type())) {
        fmt::print(out_, " (primitive)");
    }
    fmt::println(out_, "");
}

auto dumper::visit(node_id, const if_expr& if_expr) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "IfExpression");
    {
        const indent::guard g{indent_, false};
        fmt::println(
            out_, "{}Constexpr: {}", indent_.current_branch(), if_expr.constexpr_condition);
    }

    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Condition: ", indent_.current_branch());
        dump(if_expr.condition);
    }

    const auto has_alternate{if_expr.alternate.has_value()};
    {
        indent::guard g{indent_, !has_alternate};
        fmt::print(out_, "{}Consequence: ", indent_.current_branch());
        dump(if_expr.consequence);
    }

    if (has_alternate) {
        indent::guard g{indent_, true};
        fmt::print(out_, "{}Alternate: ", indent_.current_branch());
        dump(*if_expr.alternate);
    }
}

auto dumper::visit(node_id, const index_expr& index) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "IndexExpression");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Object: ", indent_.current_branch());
        dump(index.array);
    }
    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Index: ", indent_.current_branch());
        dump(index.index);
    }
}

auto dumper::visit(node_id, const infinite_loop_expr& loop) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "InfiniteLoopExpression");
    const auto& block{ast_.get_as<block_stmt>(*loop.block)};
    dump_node_list(block);
}

auto dumper::visit(node_id, const cfg_value_expr& cfg_value) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "CfgValueExpression");
    if (cfg_value.predicate) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Predicate: ", indent_.current_branch());
        dump(*cfg_value.predicate);
        return;
    }

    for (const auto& guard : cfg_value.guards) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Guard: ", indent_.current_branch());
        dump(guard.predicate);
        {
            const indent::guard g2{indent_, true};
            fmt::print(out_, "{}=> ", indent_.current_branch());
            dump(guard.value);
        }
    }

    if (cfg_value.fallback) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}_ => ", indent_.current_branch());
        dump(*cfg_value.fallback);
    }
}

#define MAKE_INFIX_DUMP(NodeType, Name, LeftLabel, RightLabel)                         \
    auto dumper::visit(node_id id, const NodeType& node) -> void {                     \
        PROFILE_FUNCTION();                                                            \
        fmt::println(out_, #Name " ({})", magic_enum::enum_name(id.get_token_type())); \
        {                                                                              \
            const indent::guard g{indent_, false};                                     \
            fmt::print(out_, "{}" #LeftLabel ": ", indent_.current_branch());          \
            dump(node.lhs);                                                            \
        }                                                                              \
        {                                                                              \
            const indent::guard g{indent_, true};                                      \
            fmt::print(out_, "{}" #RightLabel ": ", indent_.current_branch());         \
            dump(node.rhs);                                                            \
        }                                                                              \
    }

MAKE_INFIX_DUMP(assignment_expr, AssignmentExpression, Assignee, Value)
MAKE_INFIX_DUMP(binary_expr, BinaryExpression, LHS, RHS)

auto dumper::visit(node_id id, const dot_expr& node) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "DotExpression ({})", magic_enum::enum_name(id.get_token_type()));
    {
        const indent::guard g{indent_, false};
        fmt ::print(out_, "{}Object: ", indent_.current_branch());
        dump(node.object);
    }
    {
        const indent ::guard g{indent_, true};
        fmt ::print(out_, "{}Member: ", indent_.current_branch());
        dump(node.member);
    }
}

MAKE_INFIX_DUMP(range_expr, RangeExpression, Lower, Upper)

auto dumper::visit(node_id, const initializer_expr& init) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "Initializer Expression:");
    const auto has_initializers{!init.initializers.empty()};
    {
        const indent::guard g{indent_, !has_initializers};
        fmt::print(out_, "{}Object Type: ", indent_.current_branch());
        if (init.object_type) {
            dump(*init.object_type);
        } else {
            fmt::println(out_, "<inferred>");
        }
    }

    if (has_initializers) {
        const indent::guard g{indent_, true};
        fmt::println(out_, "{}Initializers:", indent_.current_branch());
        dump_container(init.initializers,
                       [this](const initializer_expr::initializer& initializer) -> void {
                           fmt::println(out_, "{}Initializer:", indent_.current_branch());
                           if (initializer.member) {
                               const indent::guard g_inner{indent_, false};
                               fmt::print(out_, "{}Member: ", indent_.current_branch());
                               dump(*initializer.member);
                           }

                           {
                               const indent::guard g_inner{indent_, true};
                               fmt::print(out_, "{}Value: ", indent_.current_branch());
                               dump(initializer.value);
                           }
                       });
    }
}

auto dumper::visit(node_id, const label_expr& label) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "Label Expression:");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Label: ", indent_.current_branch());
        dump(label.name);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Body: ", indent_.current_branch());
        dump(label.body);
    }
}

auto dumper::visit(node_id, const match_expr& match) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "MatchExpression");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Matcher: ", indent_.current_branch());
        dump(match.matcher);
    }

    {
        const indent::guard g{indent_, true};
        usize               idx{0};

        fmt::println(out_, "{}Arms:", indent_.current_branch());
        dump_container(match.arms, [&](const match_expr::arm& arm) -> void {
            if (match.catch_all_idx && idx == *match.catch_all_idx) {
                fmt::println(out_, "{}Catch-All Arm:", indent_.current_branch());
            } else {
                fmt::println(out_, "{}Arm:", indent_.current_branch());
            }
            idx += 1;

            {
                const indent::guard g_inner{indent_, false};
                fmt::print(out_, "{}Pattern: ", indent_.current_branch());
                dump(arm.pattern);
            }

            if (arm.capture) {
                const indent::guard g_inner{indent_, false};
                fmt::print(out_, "{}Capture: ", indent_.current_branch());
                if (arm.capture->is<ast::discarded>()) {
                    fmt::println(out_, "<discarded>");
                } else {
                    const auto& ident{ast_.get_as<identifier_expr>(*arm.capture)};
                    fmt::println(out_, "{} (modifier: {})", ident, arm.modifier);
                }
            }

            {
                const indent::guard g_inner{indent_, true};
                fmt::print(out_, "{}Dispatch: ", indent_.current_branch());
                dump(arm.dispatch);
            }
        });
    }
}

#define MAKE_PREFIX_DUMP(NodeType, Name)                                               \
    auto dumper::visit(node_id id, const NodeType& node) -> void {                     \
        PROFILE_FUNCTION();                                                            \
        fmt::println(out_, #Name " ({})", magic_enum::enum_name(id.get_token_type())); \
        const indent::guard g{indent_, true};                                          \
        fmt::print(out_, "{}Operand: ", indent_.current_branch());                     \
        dump(node.rhs);                                                                \
    }

MAKE_PREFIX_DUMP(reference_expr, ReferenceExpression)
MAKE_PREFIX_DUMP(address_of_expr, AddressOfExpression)
MAKE_PREFIX_DUMP(dereference_expr, DereferenceExpression)
MAKE_PREFIX_DUMP(unary_expr, UnaryExpression)

auto dumper::visit(node_id id, const unwrap_expr& node) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "UnwrapExpression ({})", magic_enum::enum_name(id.get_token_type()));
    const indent::guard g{indent_, true};
    fmt::print(out_, "{}Operand: ", indent_.current_branch());
    dump(node.operand);
}

auto dumper::visit(node_id, const implicit_access_expr& implicit_access) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ImplicitAccessExpression");
    const indent::guard g{indent_, true};
    fmt::print(out_, "{}Member: ", indent_.current_branch());
    dump(implicit_access.member);
}

#define MAKE_LEAF_DUMP(NodeType, Name)                          \
    auto dumper::visit(node_id, const NodeType& node) -> void { \
        PROFILE_FUNCTION();                                     \
        fmt::println(out_, #Name ": {}", node);                 \
    }

MAKE_LEAF_DUMP(string_expr, StringExpression)
MAKE_LEAF_DUMP(i32_expr, I32Expression)
MAKE_LEAF_DUMP(i64_expr, I64Expression)
MAKE_LEAF_DUMP(isize_expr, ISizeExpression)
MAKE_LEAF_DUMP(u32_expr, U32Expression)
MAKE_LEAF_DUMP(u64_expr, U64Expression)
MAKE_LEAF_DUMP(usize_expr, USizeExpression)
MAKE_LEAF_DUMP(u8_expr, U8Expression)
MAKE_LEAF_DUMP(f32_expr, F32Expression)
MAKE_LEAF_DUMP(f64_expr, F64Expression)

auto dumper::visit(node_id id, const bool_expr&) -> void {
    PROFILE_FUNCTION();
    fmt::println(
        out_, "BoolExpression: {}", id.get_token_type() == syntax::token_type_t::BOOLEAN_TRUE);
}

auto dumper::visit(node_id, const void_expr&) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "VoidExpression");
}

auto dumper::visit(node_id, const undefined_expr&) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "UndefinedExpression");
}

auto dumper::visit(node_id, const nullptr_expr&) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "NullptrExpression");
}

auto dumper::visit(node_id, const unreachable_expr&) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "UnreachableExpression");
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const module_access_expr& module_access) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ModuleAccessExpression");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Outer: ", indent_.current_branch());
        dump(module_access.outer);
    }
    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Inner: ", indent_.current_branch());
        dump(module_access.inner);
    }
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const struct_expr& node) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "StructExpression");

    const auto has_fields{!node.fields.empty()};
    const auto has_cfg{!node.cfg_groups.empty()};
    const auto has_members{!node.members.empty()};

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Extern: {}", indent_.current_branch(), node.is_extern);
    }
    {
        const indent::guard g{indent_, !has_fields && !has_cfg && !has_members};
        fmt::println(out_, "{}Packed: {}", indent_.current_branch(), node.is_packed);
    }

    if (has_fields) {
        const indent::guard g{indent_, !has_cfg && !has_members};
        fmt::println(out_, "{}Fields:", indent_.current_branch());
        dump_container(node.fields, [this](const struct_expr::field& field) -> void {
            {
                fmt::print(out_, "{}Name: ", indent_.current_branch());
                dump(field.name);
            }

            {
                const indent::guard g_ident{indent_, false};
                fmt::println(out_, "{}Public: {}", indent_.current_branch(), field.is_public());
            }

            const auto has_default{field.default_value.has_value()};
            const auto has_alignment{field.explicit_alignment.has_value()};
            {
                const indent::guard g_type{indent_, !has_default && !has_alignment};
                fmt::print(out_, "{}Type: ", indent_.current_branch());
                dump(field.explicit_type);
            }

            if (has_default) {
                const indent::guard g_val{indent_, !has_alignment};
                fmt::print(out_, "{}Default: ", indent_.current_branch());
                dump(*field.default_value);
            }

            if (has_alignment) {
                const indent::guard g_align{indent_, true};
                fmt::print(out_, "{}Alignment: ", indent_.current_branch());
                dump(*field.explicit_alignment);
            }
        });
    }

    const auto has_member_cfg{!node.member_cfg_groups.empty()};
    if (has_cfg) {
        const indent::guard g{indent_, !has_members && !has_member_cfg};
        dump_cfg_groups(node.cfg_groups);
    }

    if (has_members) {
        const indent::guard g{indent_, !has_member_cfg};
        fmt::println(out_, "{}Members:", indent_.current_branch());
        dump_node_list(node.members);
    }

    if (has_member_cfg) {
        const indent::guard g{indent_, true};
        dump_cfg_groups(node.member_cfg_groups);
    }
}

// Safe to call with invalid ID in type dispatch
auto dumper::visit(node_id, const union_expr& node) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "UnionExpression");

    const auto has_cfg{!node.cfg_groups.empty()};
    const auto has_members{!node.members.empty()};
    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Extern: {}", indent_.current_branch(), node.is_extern);
    }
    {
        const indent::guard g{indent_, !has_cfg && !has_members};
        fmt::println(out_, "{}Fields:", indent_.current_branch());
        dump_container(node.fields, [this](const union_expr::field& field) -> void {
            fmt::println(out_, "{}Field:", indent_.current_branch());
            {
                const indent::guard g_tag{indent_, false};
                fmt::print(out_, "{}Tag: ", indent_.current_branch());
                dump(field.name);
            }

            const auto has_alignment{field.explicit_alignment.has_value()};
            {
                const indent::guard g_result{indent_, !has_alignment};
                fmt::print(out_, "{}Type: ", indent_.current_branch());
                dump(field.explicit_type);
            }

            if (has_alignment) {
                const indent::guard g_align{indent_, true};
                fmt::print(out_, "{}Alignment: ", indent_.current_branch());
                dump(*field.explicit_alignment);
            }
        });
    }

    const auto has_member_cfg{!node.member_cfg_groups.empty()};
    if (has_cfg) {
        const indent::guard g{indent_, !has_members && !has_member_cfg};
        dump_cfg_groups(node.cfg_groups);
    }

    if (has_members) {
        const indent::guard g{indent_, !has_member_cfg};
        fmt::println(out_, "{}Members:", indent_.current_branch());
        dump_node_list(node.members);
    }

    if (has_member_cfg) {
        const indent::guard g{indent_, true};
        dump_cfg_groups(node.member_cfg_groups);
    }
}

auto dumper::visit(node_id, const while_loop_expr& while_expr) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "WhileLoopExpression");
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Condition: ", indent_.current_branch());
        dump(while_expr.condition);
    }

    if (while_expr.continuation) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Continuation: ", indent_.current_branch());
        dump(*while_expr.continuation);
    }

    const auto has_non_break{while_expr.non_break.has_value()};
    {
        const indent::guard g{indent_, !has_non_break};
        fmt::print(out_, "{}Body: ", indent_.current_branch());
        dump(while_expr.block);
    }

    if (has_non_break) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Non-Break Clause: ", indent_.current_branch());
        dump(*while_expr.non_break);
    }
}

auto dumper::visit(node_id, const block_stmt& block) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "BlockStatement");
    if (block.empty()) {
        const indent::guard g{indent_, true};
        fmt::println(out_, "{}<empty>", indent_.current_branch());
    } else {
        dump_node_list(block);
    }
}

auto dumper::visit(node_id, const break_stmt& break_stmt) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "BreakStatement");
    const auto has_expression{break_stmt.expression.has_value()};
    if (break_stmt.label) {
        const indent ::guard g{indent_, !has_expression};
        fmt::print(out_, "{}Label: ", indent_.current_branch());
        dump(*break_stmt.label);
    }

    if (has_expression) {
        const indent ::guard g{indent_, true};
        fmt::print(out_, "{}Value: ", indent_.current_branch());
        dump(*break_stmt.expression);
    }
}

auto dumper::visit(node_id, const cfg_stmt& cfg) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "CfgStatement");
    for (auto it{cfg.arms.begin()}; it != cfg.arms.end(); ++it) {
        const indent::guard g{indent_, std::next(it) == cfg.arms.end()};
        if (it->predicate) {
            fmt::print(out_, "{}Arm: ", indent_.current_branch());
            dump(*it->predicate);
        } else {
            fmt::println(out_, "{}Else:", indent_.current_branch());
        }

        for (auto item{it->items.begin()}; item != it->items.end(); ++item) {
            const indent::guard g2{indent_, std::next(item) == it->items.end()};
            fmt::print(out_, "{}", indent_.current_branch());
            dump(*item);
        }
    }
}

auto dumper::visit(node_id, const continue_stmt& continue_stmt) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ContinueStatement");
    if (continue_stmt.label) {
        const indent ::guard g{indent_, true};
        fmt::print(out_, "{}Label: ", indent_.current_branch());
        dump(*continue_stmt.label);
    }
}

auto dumper::visit(node_id, const decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "DeclStatement");

    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Name: ", indent_.current_branch());
        dump(decl.name);
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_,
                     "{}Modifiers: {}",
                     indent_.current_branch(),
                     magic_enum::enum_flags_name(decl.modifiers));
    }

    if (decl.extern_target) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Extern Target: ", indent_.current_branch());
        dump(*decl.extern_target);
    }

    if (decl.link_name) {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Link Name: ", indent_.current_branch());
        dump(*decl.link_name);
    }

    const auto has_value{decl.value.has_value()};
    {
        const indent::guard g{indent_, !has_value};
        fmt::print(out_, "{}Type: ", indent_.current_branch());
        if (decl.explicit_type) {
            dump(*decl.explicit_type);
        } else {
            fmt::println(out_, "(inferred)");
        }
    }

    if (has_value) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Value: ", indent_.current_branch());
        dump(*decl.value);
    }
}

#define MAKE_BASIC_STMT_DUMP(NodeType, Name, FieldName, field)                \
    auto dumper::visit(node_id, const NodeType& node) -> void {               \
        PROFILE_FUNCTION();                                                   \
        fmt::println(out_, #Name);                                            \
        {                                                                     \
            const indent::guard g{indent_, true};                             \
            fmt::print(out_, "{}" #FieldName ": ", indent_.current_branch()); \
            dump(node.field);                                                 \
        }                                                                     \
    }

MAKE_BASIC_STMT_DUMP(defer_stmt, DeferStatement, Deferred, deferred)
MAKE_BASIC_STMT_DUMP(discard_stmt, DiscardStatement, Discarded, discarded)
MAKE_BASIC_STMT_DUMP(expr_stmt, ExpressionStatement, Expr, expression)

auto dumper::visit(node_id id, const import_stmt& import_stmt) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ImportStatement");
    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Public: {}", indent_.current_branch(), import_stmt::is_public(id));
    }

    const auto has_alias{import_stmt.alias.has_value()};
    {
        const indent::guard g{indent_, !has_alias};
        if (import_stmt.payload.is<identifier_expr>()) {
            fmt::print(out_, "{}Library: ", indent_.current_branch());
        } else {
            fmt::print(out_, "{}File: ", indent_.current_branch());
        }
        dump(import_stmt.payload);
    }

    if (has_alias) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Alias: ", indent_.current_branch());
        dump(*import_stmt.alias);
    }
}

auto dumper::visit(node_id, const return_stmt& return_stmt) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "ReturnStatement");
    if (return_stmt.expression) {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Value: ", indent_.current_branch());
        dump(*return_stmt.expression);
    }
}

auto dumper::visit(node_id, const test_stmt& test) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "TestStatement");
    if (test.description) {
        const indent::guard g{indent_, false};
        const auto&         string{ast_.get_as<string_expr>(**test.description)};
        fmt::println(out_, "{}Description: {}", indent_.current_branch(), string);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Block: ", indent_.current_branch());
        dump(*test.block);
    }
}

auto dumper::visit(node_id id, const using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "UsingStatement");
    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Public: {}", indent_.current_branch(), using_stmt::is_public(id));
        fmt::print(out_, "{}Alias: ", indent_.current_branch());
        dump(using_stmt.alias);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Type: ", indent_.current_branch());
        dump(using_stmt.explicit_type);
    }
}

#define MAKE_EXPLICIT_TYPE_DUMP(TypeData)                                \
    auto dumper::visit(explicit_type_id, const TypeData& type) -> void { \
        PROFILE_FUNCTION();                                              \
        fmt::print(out_, "{}", indent_.current_branch());                \
        visit(node_id::make_invalid(), type);                            \
    }

auto dumper::visit(explicit_type_id id, const identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    fmt::print(out_, "{}", indent_.current_branch());
    fmt::print(out_, "IdentifierExpression: {}", ident);
    if (syntax::get_builtin_opt(id.get_token_type())) {
        fmt::print(out_, " (builtin)");
    } else if (syntax::token_type::is_primitive(id.get_token_type())) {
        fmt::print(out_, " (primitive)");
    }
    fmt::println(out_, "");
}

MAKE_EXPLICIT_TYPE_DUMP(module_access_expr)
MAKE_EXPLICIT_TYPE_DUMP(dot_expr)
MAKE_EXPLICIT_TYPE_DUMP(call_expr)

auto dumper::visit(explicit_type_id, const explicit_function_type& function) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "{}FunctionExpression", indent_.current_branch());
    if (!function.parameter_types.empty()) {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Parameters:", indent_.current_branch());
        dump_container(function.parameter_types, [this](explicit_type_id type) -> void {
            fmt::println(out_, "{}Param:", indent_.current_branch());
            {
                const indent::guard g_type{indent_, true};
                fmt::print(out_, "{}Type: ", indent_.current_branch());
                dump(type);
            }
        });
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(out_, "{}Variadic: {}", indent_.current_branch(), function.variadic);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}Returns: ", indent_.current_branch());
        dump(function.explicit_return_type);
    }
}

auto dumper::visit(explicit_type_id, const explicit_type_id& recursive) -> void {
    PROFILE_FUNCTION();
    fmt::print(out_, "{}", indent_.current_branch());
    dump(recursive);
}

MAKE_EXPLICIT_TYPE_DUMP(struct_expr)
MAKE_EXPLICIT_TYPE_DUMP(enum_expr)
MAKE_EXPLICIT_TYPE_DUMP(union_expr)

auto dumper::visit(explicit_type_id, const explicit_array_type& array) -> void {
    PROFILE_FUNCTION();
    fmt::println(out_, "{}ArrayType", indent_.current_branch());
    {
        const indent::guard g{indent_, false};
        fmt::print(out_, "{}Dimensions: ", indent_.current_branch());
        if (array.dimension) {
            dump(*array.dimension);
        } else {
            fmt::println(out_, "(slice)");
        }
    }

    {
        const indent::guard g{indent_, false};
        fmt::println(
            out_, "{}Null terminated: {}", indent_.current_branch(), array.null_terminated);
    }

    {
        const indent::guard g{indent_, true};
        fmt::print(out_, "{}", indent_.current_branch());
        dump(array.inner_explicit_type);
    }
}

} // namespace ghoti::ast
