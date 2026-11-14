#include <string_view>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "astgen_internal.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir::detail {

auto astgen::stmt(ast::stmt_handle h) -> bool {
    const ast::node_id id{h};
    using ast::node_kind;
    switch (id.get_kind()) {
    case node_kind::DECL_STATEMENT: local_decl(id, ast_.get_as<ast::decl_stmt>(id)); return false;
    case node_kind::EXPRESSION_STATEMENT: {
        const auto expression{ast_.get_as<ast::expr_stmt>(id).expression};
        DISCARD(expr(expression));
        return expression.is<ast::unreachable_expr>();
    }
    case node_kind::BLOCK_STATEMENT: {
        const auto& block{ast_.get_as<ast::block_stmt>(id)};
        const auto  body{scoped_block(ast::block_handle{id})};
        DISCARD(add_pl(block.is_comptime ? tag::block_comptime : tag::block,
                       writer().str_field({}).body_field(body),
                       id));
        return false;
    }
    case node_kind::BREAK_STATEMENT: break_stmt(id, ast_.get_as<ast::break_stmt>(id)); return true;
    case node_kind::CONTINUE_STATEMENT:
        continue_stmt(id, ast_.get_as<ast::continue_stmt>(id));
        return true;
    case node_kind::RETURN_STATEMENT:
        return_stmt(id, ast_.get_as<ast::return_stmt>(id));
        return true;
    case node_kind::DEFER_STATEMENT:
    case node_kind::ERRDEFER_STATEMENT: push_defer(id, id.is<ast::errdefer_stmt>()); return false;
    case node_kind::DISCARD_STATEMENT:
        DISCARD(add_un(tag::discard, expr(ast_.get_as<ast::discard_stmt>(id).discarded), 0, id));
        return false;
    case node_kind::IMPORT_STATEMENT: {
        const auto& import{ast_.get_as<ast::import_stmt>(id)};
        const auto  index{add_import(id, import)};
        const auto  value{add_un(tag::import, stdx::none, std::to_underlying(index), id)};
        if (const auto named{import.get_name(ast_)}) {
            DISCARD(declare(named->second,
                            import_location(id, import),
                            {.kind = binding_kind::VALUE, .value = value}));
        }
        return false;
    }
    case node_kind::TEST_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_TEST_LOCATION,
               "Tests must be at the topmost level of a file");
        return false;
    case node_kind::IMPL_STATEMENT: {
        // An impl is a module-level item wherever it's written; the old pipeline accepts it here
        decl_info  d{.kind = decl_kind::IMPL, .node = id};
        const auto index{out_.add_decl(d)};
        out_.root_namespace_mut().decls.emplace_back(index);
        lower_impl(index, ast_.get_as<ast::impl_stmt>(id));
        return false;
    }
    case node_kind::CFG_STATEMENT:
        return block_statements(selected_items(id, ast_.get_as<ast::cfg_stmt>(id)));
    default: UNREACHABLE("every statement kind has a lowering");
    }
}

auto astgen::block_statements(const std::vector<ast::stmt_handle>& statements) -> bool {
    bool diverged{false};
    for (const auto statement : statements) { diverged = stmt(statement) || diverged; }
    return diverged;
}

auto astgen::scoped_block(ast::block_handle h, scope_kind kind) -> std::vector<inst_id> {
    const scope_guard scoped{*this, kind};
    return block_in_current_scope(h);
}

auto astgen::block_in_current_scope(ast::block_handle h) -> std::vector<inst_id> {
    const auto& block{ast_.get_as<ast::block_stmt>(h)};
    const auto  statements{spliced(block.statements)};
    announce_later(statements);

    auto&                state{functions_.back()};
    const auto           height{state.defers.size()};
    std::vector<inst_id> body;
    {
        const body_guard guard{*this, body};
        if (!block_statements(statements)) { emit_defers(height, stdx::none); }
    }
    auto& defers{functions_.back().defers};
    defers.erase(defers.begin() + static_cast<idiff>(height), defers.end());
    return body;
}

auto astgen::local_decl(ast::node_id id, const ast::decl_stmt& decl) -> void {
    check_decl_modifiers(id, decl, in_function());
    using ast::decl_modifiers;
    const auto name{name_of(decl.name)};
    const auto type_ref{optional_type(decl.explicit_type)};
    const bool is_comptime{decl.has_modifier(decl_modifiers::COMPTIME)};
    const bool is_mut{decl.has_modifier(decl_modifiers::MUT)};

    binding    b{};
    const auto initializes_fn{decl.value ? ast_.get_as_opt<ast::function_expr>(*decl.value)
                                         : stdx::none};
    if (!is_mut && initializes_fn) {
        // A local function can call itself, so its name is bound before its body
        const auto inst{reserve(id)};
        DISCARD(declare(
            name, location_of(decl.name), {.kind = binding_kind::VALUE, .value = inst_ref(inst)}));
        const ast::node_id value_node{*decl.value};
        if (!is_comptime) {
            const auto init{lower_function(value_node, *initializes_fn, decl)};
            fill_pl(
                inst, tag::let_value, writer().str_field(name).ref_field(type_ref).ref_field(init));
            return;
        }
        std::vector<inst_id> value;
        {
            const body_guard guard{*this, value};
            yields(lower_function(value_node, *initializes_fn, decl), value_node);
        }
        fill_pl(
            inst, tag::const_local, writer().str_field(name).ref_field(type_ref).body_field(value));
        return;
    }
    if (is_comptime && !is_mut) {
        std::vector<inst_id> value;
        if (decl.value) {
            const body_guard guard{*this, value};
            const auto       initializer{*decl.value};
            ref              result{};
            if (const auto fn{ast_.get_as_opt<ast::function_expr>(initializer)}) {
                result = lower_function(ast::node_id{initializer}, *fn, decl);
            } else {
                if (initializer.any<ast::struct_expr,
                                    ast::union_expr,
                                    ast::enum_expr,
                                    ast::interface_expr>()) {
                    pending_type_name_ = name;
                }
                result             = expr(initializer);
                pending_type_name_ = {};
            }
            yields(result, ast::node_id{initializer});
        }
        b = {.kind  = binding_kind::VALUE,
             .value = add_pl(tag::const_local,
                             writer().str_field(name).ref_field(type_ref).body_field(value),
                             id)};
    } else if (is_mut) {
        const auto slot{add_pl(is_comptime ? tag::alloc_comptime : tag::alloc_mut,
                               writer().str_field(name).ref_field(type_ref),
                               id)};
        if (decl.value) { DISCARD(add_bin(tag::store, slot, expr(*decl.value), id)); }
        b = {.kind = binding_kind::PLACE, .value = slot};
    } else {
        const auto init{decl.value ? expr(*decl.value) : to_ref(well_known::undefined)};
        b = {.kind  = binding_kind::VALUE,
             .value = add_pl(
                 tag::let_value, writer().str_field(name).ref_field(type_ref).ref_field(init), id)};
    }
    DISCARD(declare(name, location_of(decl.name), b));
}

auto astgen::find_target(ast::node_id                                id,
                         const stdx::option<ast::identifier_handle>& label,
                         bool                                        loops_only,
                         std::string_view stmt_name) -> stdx::option<jump_target> {
    const auto& state{functions_.back()};
    const auto  report_unknown{[&] {
        if (label) {
            report(location_of(*label),
                   sema::error::ILLEGAL_CONTROL_FLOW,
                   "Labeled {} statements must be used with a known label",
                   stmt_name);
        } else if (loops_only) {
            report(location_of(id),
                   sema::error::ILLEGAL_CONTROL_FLOW,
                   "Cannot continue outside of a loop");
        } else {
            report(location_of(id),
                   sema::error::ILLEGAL_CONTROL_FLOW,
                   "Cannot break outside of a loop or label");
        }
    }};

    for (usize i{state.targets.size()}; i-- > 0;) {
        if (label && state.targets[i].label.value_or(std::string_view{}) != name_of(*label)) {
            continue;
        }
        if (loops_only && !state.targets[i].is_loop) {
            if (!label) { continue; }
            // `continue :blk` on a plain block continues the loop around the block
            while (i > 0 && !state.targets[i].is_loop) { --i; }
            if (!state.targets[i].is_loop) { break; }
        }
        const auto& target{state.targets[i]};
        if (i < state.target_floor) {
            report(location_of(id),
                   sema::error::ILLEGAL_CONTROL_FLOW,
                   "A {} cannot leave a defer body",
                   stmt_name);
            return stdx::none;
        }
        return target;
    }
    report_unknown();
    return stdx::none;
}

auto astgen::break_stmt(ast::node_id id, const ast::break_stmt& node) -> void {
    const auto target{find_target(id, node.label, false, "break")};
    const auto value{node.expression ? expr(*node.expression) : to_ref(well_known::void_value)};
    if (!target) { return; }
    emit_defers(target->defer_height, stdx::none);
    DISCARD(add_bin(tag::break_to, inst_ref(target->inst), value, id));
}

auto astgen::continue_stmt(ast::node_id id, const ast::continue_stmt& node) -> void {
    const auto target{find_target(id, node.label, true, "continue")};
    if (!target) { return; }
    emit_defers(target->defer_height, stdx::none);
    DISCARD(add_un(tag::repeat, inst_ref(target->inst), 0, id));
}

auto astgen::return_stmt(ast::node_id id, const ast::return_stmt& node) -> void {
    const auto value{node.expression ? expr(*node.expression) : to_ref(well_known::void_value)};
    if (!in_function()) {
        report(location_of(id),
               sema::error::ILLEGAL_CONTROL_FLOW,
               "Cannot return outside of a function");
        return;
    }
    if (functions_.back().in_defer) {
        report(location_of(id),
               sema::error::ILLEGAL_CONTROL_FLOW,
               "A return cannot leave a defer body");
        return;
    }
    emit_return(id, value);
}

} // namespace ghoti::uir::detail
