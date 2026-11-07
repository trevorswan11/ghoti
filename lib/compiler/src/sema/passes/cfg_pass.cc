#include "compiler/sema/passes/cfg_pass.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/cfg_atoms.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

auto cfg_pass::run(mod::module_t& module, context& ctx) -> bool {
    PROFILE_FUNCTION();
    cfg_pass pass{module, ctx};
    pass.eval_.gather_cfg_values(module.ast.get_roots());
    pass.eval_.resolve_all_cfg_values();
    pass.rewrite_roots(module.ast.get_roots());
    return pass.eval_.is_ok();
}

cfg_pass::cfg_pass(mod::module_t& module, context& ctx)
    : module_{module}, cx_{cfg_context::from(ctx)}, eval_{module, cx_, ctx.diags} {
    eval_.record_results_on(module);
}

auto cfg_pass::rewrite_items(std::vector<ast::stmt_handle>& list) -> void {
    PROFILE_FUNCTION();
    std::vector<ast::stmt_handle> out;
    out.reserve(list.size());
    for (const auto stmt : list) {
        if (const auto cfg{module_.ast.get_as_opt<ast::cfg_stmt>(stmt)}) {
            if (const auto arm{eval_.select_arm_of(cfg->arms)}) {
                auto items{arm->items};
                rewrite_items(items);
                eval_.fire_eager_compile_errors(items);
                for (const auto spliced : items) { out.emplace_back(spliced); }
            }
            continue; // the @cfg node itself is dropped
        }
        recurse_into_bodies(stmt);
        out.emplace_back(stmt);
    }
    list = std::move(out);
}

auto cfg_pass::rewrite_roots(std::vector<ast::node_id>& roots) -> void {
    PROFILE_FUNCTION();
    std::vector<ast::stmt_handle> as_items;
    as_items.reserve(roots.size());
    for (const auto id : roots) { as_items.emplace_back(ast::stmt_handle{id}); }
    rewrite_items(as_items);
    roots.assign(as_items.begin(), as_items.end());
}

auto cfg_pass::recurse_into_bodies(ast::stmt_handle stmt) -> void {
    PROFILE_FUNCTION();
    const ast::node_id id{stmt};
    if (id.get_kind() == ast::node_kind::BLOCK_STATEMENT) {
        rewrite_items(module_.ast.get_as_mut<ast::block_stmt>(id).statements);
        return;
    }
    if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(id)}; decl && decl->value) {
        recurse_into_expr(*decl->value);
    }
    if (const auto expr_st{module_.ast.get_as_opt<ast::expr_stmt>(id)}) {
        recurse_into_expr(expr_st->expression);
    }
    if (const auto ret{module_.ast.get_as_opt<ast::return_stmt>(id)}; ret && ret->expression) {
        recurse_into_expr(*ret->expression);
    }
    if (const auto brk{module_.ast.get_as_opt<ast::break_stmt>(id)}; brk && brk->expression) {
        recurse_into_expr(*brk->expression);
    }
    if (const auto discard{module_.ast.get_as_opt<ast::discard_stmt>(id)}) {
        recurse_into_expr(discard->discarded);
    }
    if (const auto defer{module_.ast.get_as_opt<ast::defer_stmt>(id)}) {
        recurse_into_bodies(defer->deferred);
    }
    if (const auto errdefer{module_.ast.get_as_opt<ast::errdefer_stmt>(id)}) {
        recurse_into_bodies(errdefer->deferred);
    }
    if (const auto test{module_.ast.get_as_opt<ast::test_stmt>(id)}) {
        rewrite_items(
            module_.ast.get_as_mut<ast::block_stmt>(ast::node_id{test->block}).statements);
    }
}

auto cfg_pass::recurse_into_block(ast::block_handle block) -> void {
    PROFILE_FUNCTION();
    rewrite_items(module_.ast.get_as_mut<ast::block_stmt>(ast::node_id{block}).statements);
}

auto cfg_pass::recurse_into_type(ast::explicit_type_id type) -> void {
    switch (type.get_kind()) {
    case ast::explicit_type_kind::EXPR:
        recurse_into_expr(module_.ast.get_as<ast::explicit_expr_type>(type).expr);
        break;
    case ast::explicit_type_kind::RECURSIVE:
        recurse_into_type(module_.ast.get_as<ast::explicit_type_id>(type));
        break;
    case ast::explicit_type_kind::ARRAY: {
        const auto& array{module_.ast.get_as<ast::explicit_array_type>(type)};
        if (array.dimension) { recurse_into_expr(*array.dimension); }
        recurse_into_type(array.inner_explicit_type);
        break;
    }
    default: break;
    }
}

auto cfg_pass::recurse_into_members(const ast::member_list& members) -> void {
    PROFILE_FUNCTION();
    for (const auto member : members) {
        const ast::node_id mid{member};
        if (const auto decl{module_.ast.get_as_opt<ast::decl_stmt>(mid)}; decl && decl->value) {
            recurse_into_expr(*decl->value);
        }
    }
}

auto cfg_pass::recurse_into_expr(ast::expr_handle expr) -> void {
    PROFILE_FUNCTION();
    // An aggregate's gated fields and members are spliced in place
    const auto flatten = [this](auto& node, auto& items) {
        flatten_cfg_groups(node.cfg_groups, items);
        flatten_cfg_groups(node.member_cfg_groups, node.members);
        recurse_into_members(node.members);
    };
    const ast::node_id id{expr};
    if (id.is<ast::struct_expr>()) {
        auto& node{module_.ast.get_as_mut<ast::struct_expr>(id)};
        return flatten(node, node.fields);
    }
    if (id.is<ast::union_expr>()) {
        auto& node{module_.ast.get_as_mut<ast::union_expr>(id)};
        return flatten(node, node.fields);
    }
    if (id.is<ast::enum_expr>()) {
        auto& node{module_.ast.get_as_mut<ast::enum_expr>(id)};
        return flatten(node, node.enumerations);
    }

    // Any operand can hold a block-bodied `if`, `match`, loop, or function literal
    module_.ast[expr].visit(
        [&](const ast::binary_expr& bin) {
            recurse_into_expr(bin.lhs);
            recurse_into_expr(bin.rhs);
        },
        [&](const ast::assignment_expr& assign) {
            recurse_into_expr(assign.lhs);
            recurse_into_expr(assign.rhs);
        },
        [&](const ast::unary_expr& un) { recurse_into_expr(un.rhs); },
        [&](const ast::reference_expr& ref) { recurse_into_expr(ref.rhs); },
        [&](const ast::dereference_expr& deref) { recurse_into_expr(deref.rhs); },
        [&](const ast::comptime_expr& cx) { recurse_into_expr(cx.rhs); },
        [&](const ast::address_of_expr& adr) { recurse_into_expr(adr.rhs); },
        [&](const ast::unwrap_expr& unwrap) { recurse_into_expr(unwrap.operand); },
        [&](const ast::dot_expr& dot) { recurse_into_expr(dot.object); },
        [&](const ast::index_expr& idx) {
            recurse_into_expr(idx.array);
            recurse_into_expr(idx.index);
        },
        [&](const ast::range_expr& range) {
            if (range.lhs) { recurse_into_expr(*range.lhs); }
            if (range.rhs) { recurse_into_expr(*range.rhs); }
        },
        [&](const ast::call_expr& call) {
            if (call.modifier) { recurse_into_expr(*call.modifier); }
            recurse_into_expr(call.function);
            for (const auto& arg : call.arguments) {
                if (const auto value{arg.as_opt<ast::expr_handle>()}) { recurse_into_expr(*value); }
            }
        },
        [&](const ast::array_expr& array) {
            for (const auto item : array.items) { recurse_into_expr(item); }
        },
        [&](const ast::initializer_expr& init) {
            for (const auto& entry : init.initializers) { recurse_into_expr(entry.value); }
        },
        [&](const ast::function_expr& fn) {
            for (const auto& param : fn.parameters) {
                if (param.explicit_type) { recurse_into_type(*param.explicit_type); }
            }
            recurse_into_type(fn.explicit_return_type);
            if (fn.body) { recurse_into_block(*fn.body); }
        },
        [&](const ast::if_expr& branch) {
            if (branch.condition) { recurse_into_expr(*branch.condition); }
            recurse_into_bodies(branch.consequence);
            if (branch.alternate) { recurse_into_bodies(*branch.alternate); }
        },
        [&](const ast::match_expr& match) {
            recurse_into_expr(match.matcher);
            for (const auto& arm : match.arms) {
                if (arm.guard) { recurse_into_expr(*arm.guard); }
                recurse_into_bodies(arm.dispatch);
            }
        },
        [&](const ast::while_loop_expr& loop) {
            recurse_into_expr(loop.condition);
            recurse_into_block(loop.block);
            if (loop.non_break) { recurse_into_bodies(*loop.non_break); }
        },
        [&](const ast::do_while_loop_expr& loop) { recurse_into_block(loop.block); },
        [&](const ast::for_loop_expr& loop) {
            recurse_into_block(loop.block);
            if (loop.non_break) { recurse_into_bodies(*loop.non_break); }
        },
        [&](const ast::infinite_loop_expr& loop) { recurse_into_block(loop.block); },
        [&](const ast::label_expr& label) {
            const ast::node_id body{label.body};
            if (body.get_kind() == ast::node_kind::BLOCK_STATEMENT) {
                recurse_into_block(ast::block_handle{body});
            } else {
                recurse_into_expr(ast::expr_handle{body});
            }
        },
        [](const auto&) {});
}

} // namespace ghoti::sema
