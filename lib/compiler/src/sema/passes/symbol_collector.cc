#include "compiler/sema/passes/symbol_collector.hh"

#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "support/counter.hh"

namespace ghoti::sema {

auto symbol_collector::collect_symbols(mod::module& module, context& ctx) -> mod::module_state {
    PROFILE_FUNCTION();
    if (module.is_collectable()) {
        module.root_table_idx.emplace(ctx.registry.create());

        symbol_collector collector{module, ctx};
        for (const auto& node : module.ast) { collector.collect(node); }

        if (!ctx.diags.empty()) {
            return module.error_out(std::move(ctx.diags),
                                    mod::module_state::POISONED_SYMBOL_COLLECTION);
        }
        module.state = mod::module_state::SYMBOLS_COLLECTED;
    }
    return module.state;
}

// Many terminal expressions are skipped on this pass
#define MAKE_COLLECTOR_NOOPS(X) \
    X(identifier_expr)          \
    X(dot_expr)                 \
    X(implicit_access_expr)     \
    X(string_expr)              \
    X(i32_expr)                 \
    X(i64_expr)                 \
    X(isize_expr)               \
    X(u32_expr)                 \
    X(u64_expr)                 \
    X(usize_expr)               \
    X(u8_expr)                  \
    X(f32_expr)                 \
    X(f64_expr)                 \
    X(bool_expr)                \
    X(void_expr)                \
    X(undefined_expr)           \
    X(nullptr_expr)             \
    X(unreachable_expr)         \
    X(module_access_expr)

#define COLLECTOR_NOOP_X(NodeType) AST_NODE_VISITOR_NOOP(symbol_collector, NodeType)
MAKE_COLLECTOR_NOOPS(COLLECTOR_NOOP_X)
#undef COLLECTOR_NOOP_X

auto symbol_collector::visit(ast::node_id, const ast::array_expr& array) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    if (array.size) { collect(*array.size); }
    collect(array.item_explicit_type);
    for (const auto& item : array.items) { collect(*item); }
}

auto symbol_collector::visit(ast::node_id, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    collect(call.function);
    for (const auto& arg : call.arguments) {
        arg.visit([this](auto arg_id) -> void { collect(arg_id); });
    }
}

auto symbol_collector::visit(ast::node_id id, const ast::do_while_loop_expr& do_while) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g_loop{in_loop_scope_};
    const default_counter::guard g_expr{in_expr_scope_};
    const auto&                  block{collecting_.ast.get_as<ast::block_stmt>(do_while.block)};
    const auto                   idx{visit_scopes(
        type_kind::BLOCK,
        stdx::iter_pair{.iterable = block, .visitor = [this](const ast::stmt_handle& stmt) -> void {
                            collect(stmt);
                        }})};
    last_type_->set_symbol_table_idx(idx);
    collecting_.set_sema_type(id, *last_type_.take());

    // The condition is just an expression
    {
        const default_counter::guard g_cond{in_expr_scope_};
        collect(do_while.condition);
    }
}

template <ast::IndexableID ID>
auto symbol_collector::visit(ID id, const ast::enum_expr& enum_expr) -> void {
    PROFILE_FUNCTION();
    const auto scope_idx{visit_scopes(
        type_kind::ENUM,
        stdx::iter_pair{enum_expr.enumerations,
                        [this](const ast::enum_expr::enumeration& enumeration) -> void {
                            if (enumeration.value) { collect(*enumeration.value); }

                            // Resolve the ident second to prevent self-referential values
                            const auto& ident =
                                collecting_.ast.get_as<ast::identifier_expr>(enumeration.name);
                            try_declare<symbols::enumeration>(ident.name, enumeration);
                        }},
        stdx::iter_pair{enum_expr.members,
                        [this](const ast::member_handle& member) -> void { collect(*member); }})};
    last_type_->set_symbol_table_idx(scope_idx);
    collecting_.set_sema_type(id, *last_type_);
}

VISITOR_TEMPLATE_INIT(symbol_collector, visit, const ast::enum_expr&)

auto symbol_collector::visit(ast::node_id id, const ast::for_loop_expr& for_expr) -> void {
    PROFILE_FUNCTION();

    // The guard shouldn't enclose the else clause
    const default_counter::guard g_expr{in_expr_scope_};
    for (const auto& iterable : for_expr.iterables) { collect(iterable); }

    usize new_idx;
    {
        new_idx = ctx_.registry.create();
        const scope s{table_stack_, new_idx, table_idx_};

        const default_counter::guard g_loop{in_loop_scope_};
        for (const auto& capture : for_expr.captures) {
            if (const auto ident{
                    collecting_.ast.get_as_opt<ast::identifier_expr>(capture.payload)}) {
                try_declare<symbols::for_loop_capture>(ident->name, capture);
            }
        }
        const auto& block{collecting_.ast.get_as<ast::block_stmt>(for_expr.block)};
        for (const auto& stmt : block) { collect(stmt); }
    }
    if (for_expr.non_break) { collect(*for_expr.non_break); }

    last_type_.emplace(ctx_.pool[{type_kind::BLOCK, types::mut::CONSTANT, new_idx}]);
    last_type_->set_symbol_table_idx(new_idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

auto symbol_collector::visit(ast::node_id id, const ast::function_expr& fn) -> void {
    PROFILE_FUNCTION();
    const auto                   new_idx{ctx_.registry.create()};
    const scope                  s{table_stack_, new_idx, table_idx_};
    const default_counter::guard g_fn{in_function_scope_};
    const default_counter::guard g_expr{in_expr_scope_};

    // Parameters belong to the parent scope
    if (fn.self) {
        const auto& ident{collecting_.ast.get_as<ast::identifier_expr>(fn.self->name)};
        try_declare<symbols::self_parameter>(ident.name, *fn.self.get());
    }

    // The parameter's type should be collected first to prevent self-referential types
    for (const auto& param : fn.parameters) {
        collect(param.explicit_type);
        const auto& ident{collecting_.ast.get_as<ast::identifier_expr>(param.name)};
        try_declare<symbols::parameter>(ident.name, param);
    }
    collect(fn.explicit_return_type);

    const auto& block{collecting_.ast.get_as<ast::block_stmt>(fn.body)};
    for (const auto& stmt : block) { collect(stmt); }

    last_type_.emplace(ctx_.pool[{type_kind::FUNCTION, types::mut::CONSTANT, new_idx}]);
    last_type_->set_symbol_table_idx(new_idx);
    collecting_.set_sema_type(id, *last_type_);
}

auto symbol_collector::visit(ast::node_id, const ast::if_expr& if_expr) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    collect(if_expr.condition);
    collect(if_expr.consequence);
    if (if_expr.alternate) { collect(*if_expr.alternate); }
}

auto symbol_collector::visit(ast::node_id, const ast::index_expr& index) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    collect(index.index);
}

auto symbol_collector::visit(ast::node_id id, const ast::infinite_loop_expr& loop) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g_loop{in_loop_scope_};
    const default_counter::guard g_expr{in_expr_scope_};
    const auto&                  block{collecting_.ast.get_as<ast::block_stmt>(loop.block)};
    const auto                   idx{visit_scopes(
        type_kind::BLOCK,
        stdx::iter_pair{.iterable = block, .visitor = [this](const ast::stmt_handle& stmt) -> void {
                            collect(stmt);
                        }})};
    last_type_->set_symbol_table_idx(idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

#define MAKE_INFIX_COLLECTOR(NodeType)                                              \
    auto symbol_collector::visit(ast::node_id, const ast::NodeType& node) -> void { \
        PROFILE_FUNCTION();                                                         \
        const default_counter::guard g{in_expr_scope_};                             \
        collect(node.lhs);                                                          \
        collect(node.rhs);                                                          \
    }

MAKE_INFIX_COLLECTOR(range_expr)
MAKE_INFIX_COLLECTOR(assignment_expr)
MAKE_INFIX_COLLECTOR(binary_expr)

auto symbol_collector::visit(ast::node_id, const ast::initializer_expr& init) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    for (const auto& initializer : init.initializers) { collect(initializer.value); }
}

auto symbol_collector::visit(ast::node_id id, const ast::label_expr& label) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g_label{in_label_scope_};
    const default_counter::guard g_expr{in_expr_scope_};
    const auto&                  ident{collecting_.ast.get_as<ast::identifier_expr>(label.name)};

    // Labels and their associated nodes live in their own scope
    const auto  new_idx{ctx_.registry.create()};
    const scope s{table_stack_, new_idx, table_idx_};
    if (try_declare<symbols::label>(ident.name, symbols::label::handle_t{id})) {
        auto& sym{ctx_.registry.get_from(table_idx_, ident.name)};
        sym.set_kind(symbol_kind::LABEL);
    }
    collect(label.body);

    last_type_.emplace(ctx_.pool[{type_kind::LABEL, types::mut::CONSTANT, new_idx}]);
    last_type_->set_symbol_table_idx(new_idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

auto symbol_collector::visit(ast::node_id, const ast::match_expr& match) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    collect(match.matcher);
    for (const auto& arm : match.arms) {
        const auto  new_idx{ctx_.registry.create()};
        const scope s{table_stack_, new_idx, table_idx_};

        collect(arm.pattern);
        if (arm.capture && (*arm.capture)->get_kind() == ast::node_kind::IDENTIFIER_EXPRESSION) {
            const auto& ident{collecting_.ast.get_as<ast::identifier_expr>(*arm.capture)};
            try_declare<symbols::match_capture>(ident.name, *arm.capture);
        }
        collect(arm.dispatch);

        last_type_.emplace(ctx_.pool[{type_kind::MATCH_ARM, types::mut::CONSTANT, new_idx}]);
        last_type_->set_symbol_table_idx(new_idx);
        collecting_.set_sema_type(arm, *last_type_.take());
    }
}

#define MAKE_PREFIX_COLLECTOR(NodeType)                                             \
    auto symbol_collector::visit(ast::node_id, const ast::NodeType& node) -> void { \
        PROFILE_FUNCTION();                                                         \
        const default_counter::guard g{in_expr_scope_};                             \
        collect(node.rhs);                                                          \
    }

MAKE_PREFIX_COLLECTOR(reference_expr)
MAKE_PREFIX_COLLECTOR(address_of_expr)
MAKE_PREFIX_COLLECTOR(dereference_expr)
MAKE_PREFIX_COLLECTOR(unary_expr)

template <ast::IndexableID ID>
auto symbol_collector::visit(ID id, const ast::struct_expr& struct_expr) -> void {
    PROFILE_FUNCTION();
    const auto scope_idx{visit_scopes(
        type_kind::STRUCT,
        stdx::iter_pair{struct_expr.fields,
                        [this](const ast::struct_expr::field& field) -> void {
                            if (field.default_value) { collect(*field.default_value); }
                            collect(field.explicit_type);

                            // Resolve the ident last to prevent self-referential values
                            const auto& ident =
                                collecting_.ast.get_as<ast::identifier_expr>(field.name);
                            try_declare<symbols::struct_field>(ident.name, field);
                        }},
        stdx::iter_pair{struct_expr.members,
                        [this](const ast::member_handle& member) -> void { collect(*member); }})};
    last_type_->set_symbol_table_idx(scope_idx);
    collecting_.set_sema_type(id, *last_type_);
}

VISITOR_TEMPLATE_INIT(symbol_collector, visit, const ast::struct_expr&)

template <ast::IndexableID ID>
auto symbol_collector::visit(ID id, const ast::union_expr& union_expr) -> void {
    PROFILE_FUNCTION();
    const auto scope_idx{visit_scopes(
        type_kind::UNION,
        stdx::iter_pair{union_expr.fields,
                        [this](const ast::union_expr::field& field) -> void {
                            // Resolve the ident second to prevent self-referential values
                            collect(field.explicit_type);

                            const auto& ident =
                                collecting_.ast.get_as<ast::identifier_expr>(field.name);
                            try_declare<symbols::union_field>(ident.name, field);
                        }},
        stdx::iter_pair{union_expr.members,
                        [this](const ast::member_handle& member) -> void { collect(*member); }})};
    last_type_->set_symbol_table_idx(scope_idx);
    collecting_.set_sema_type(id, *last_type_);
}

VISITOR_TEMPLATE_INIT(symbol_collector, visit, const ast::union_expr&)

auto symbol_collector::visit(ast::node_id id, const ast::while_loop_expr& while_expr) -> void {
    PROFILE_FUNCTION();

    // The guard shouldn't enclose the else clause or condition
    const default_counter::guard g_expr{in_expr_scope_};
    collect(while_expr.condition);
    if (while_expr.continuation) { collect(*while_expr.continuation); }

    usize new_idx;
    {
        new_idx = ctx_.registry.create();
        const scope s{table_stack_, new_idx, table_idx_};

        const default_counter::guard g_loop{in_loop_scope_};
        const auto& block{collecting_.ast.get_as<ast::block_stmt>(while_expr.block)};
        for (const auto& stmt : block) { collect(stmt); }
    }
    if (while_expr.non_break) { collect(*while_expr.non_break); }

    last_type_.emplace(ctx_.pool[{type_kind::BLOCK, types::mut::CONSTANT, new_idx}]);
    last_type_->set_symbol_table_idx(new_idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

auto symbol_collector::visit(ast::node_id id, const ast::block_stmt& block) -> void {
    PROFILE_FUNCTION();
    if (!in_expr_scope_ && table_stack_.size() == 1) {
        ctx_.diags.emplace_back("Cannot have block at the top level",
                                error::ILLEGAL_TOP_LEVEL_STATEMENT,
                                collecting_.ast.location_of(id));
    }

    const auto scope_idx{visit_scopes(
        type_kind::BLOCK,
        stdx::iter_pair{.iterable = block, .visitor = [this](const ast::stmt_handle& stmt) -> void {
                            collect(stmt);
                        }})};
    last_type_->set_symbol_table_idx(scope_idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

auto symbol_collector::visit(ast::node_id id, const ast::break_stmt& break_stmt) -> void {
    PROFILE_FUNCTION();
    if (!in_loop_scope_ && !in_label_scope_) {
        ctx_.diags.emplace_back("Cannot break outside of a loop or label",
                                error::ILLEGAL_CONTROL_FLOW,
                                collecting_.ast.location_of(id));
    }
    if (break_stmt.expression) { collect(*break_stmt.expression); }
}

auto symbol_collector::visit(ast::node_id id, const ast::continue_stmt&) -> void {
    PROFILE_FUNCTION();
    if (!in_loop_scope_) {
        ctx_.diags.emplace_back("Cannot continue outside of a loop",
                                error::ILLEGAL_CONTROL_FLOW,
                                collecting_.ast.location_of(id));
    }
}

auto symbol_collector::visit(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();

    // We can stop analyzing early if there's no value
    const auto& ident{collecting_.ast.get_as<ast::identifier_expr>(decl.name)};
    const auto  name{ident.name};
    if (!try_declare<symbols::node_t>(name, id)) { return; };
    if (!decl.value) { return; }

    // Valued decls should be evaluated to get shallow types
    auto&      sym{ctx_.registry.get_from(table_idx_, name)};
    const auto value{*decl.value};
    if (value.any<ast::enum_expr, ast::union_expr, ast::struct_expr>()) {
        if (decl.has_modifier(ast::decl_modifiers::CONSTEXPR)) {
            ctx_.diags.emplace_back(
                fmt::format("All {}s are implicitly constexpr", value->display_name()),
                error::REDUNDANT_CONSTEXPR,
                collecting_.ast.location_of(id));
        } else if (!decl.has_modifier(ast::decl_modifiers::CONSTANT)) {
            ctx_.poison_symbol(sym,
                               fmt::format("All {}s must be marked const", value->display_name()),
                               error::ILLEGAL_NON_CONST_STATEMENT,
                               collecting_.ast.location_of(id));
        } else {
            sym.set_kind(symbol_kind::TYPE);
        }
    } else if (value.is<ast::function_expr>()) {
        if (!decl.has_modifier(ast::decl_modifiers::CONSTEXPR) &&
            !decl.has_modifier(ast::decl_modifiers::CONSTANT)) {
            ctx_.poison_symbol(sym,
                               "All function declarations must be const or constexpr",
                               error::ILLEGAL_NON_CONST_STATEMENT,
                               collecting_.ast.location_of(id));
        } else {
            sym.set_kind(symbol_kind::CALLABLE);
        }
    }

    collect(value);
    if (last_type_) { collecting_.set_sema_type(value, *last_type_.take()); }
}

auto symbol_collector::visit(ast::node_id id, const ast::defer_stmt& defer) -> void {
    PROFILE_FUNCTION();
    if (!in_function_scope_) {
        ctx_.diags.emplace_back("Cannot have defer outside of a function's scope",
                                error::ILLEGAL_TOP_LEVEL_STATEMENT,
                                collecting_.ast.location_of(id));
    }
    collect(defer.deferred);
}

auto symbol_collector::visit(ast::node_id, const ast::discard_stmt& discard) -> void {
    PROFILE_FUNCTION();
    collect(discard.discarded);
}

auto symbol_collector::visit(ast::node_id, const ast::expr_stmt& expr) -> void {
    PROFILE_FUNCTION();
    collect(expr.expression);
}

auto symbol_collector::collect_import_payload(const ast::import_stmt& import_stmt)
    -> std::pair<std::string_view, stdx::result<gsl::not_null<mod::module*>, mod::diagnostic>> {
    const auto [_, name]{import_stmt.get_name(collecting_.ast)};
    if (const auto string{collecting_.ast.get_as_opt<ast::string_expr>(import_stmt.payload)}) {
        ASSERT(import_stmt.alias, "File import without alias");
        return {name, ctx_.modules.try_get_file_module(string->value, collecting_.parent_path)};
    }

    const auto& payload{collecting_.ast.get_as<ast::identifier_expr>(import_stmt.payload)};
    return {name, ctx_.modules.try_get_library_module(payload.name)};
}

auto symbol_collector::visit(ast::node_id id, const ast::import_stmt& import_stmt) -> void {
    PROFILE_FUNCTION();
    auto [alias, mod_result]{collect_import_payload(import_stmt)};

    // Only set the table index if the module exists
    stdx::option<mod::module&> imported_mod;
    if (!mod_result) {
        // The token is retrieved here to avoid copying it on success
        ctx_.diags.emplace_back(mod_result.error().get_message(),
                                error::MODULE_LOAD_ERROR,
                                collecting_.ast.location_of(*import_stmt.payload));
    } else {
        imported_mod.emplace(**mod_result);
        context new_ctx{ctx_};
        collect_symbols(*imported_mod, new_ctx);
    }

    try_declare<symbols::node_t>(alias, id);
    if (imported_mod && !imported_mod->is_errored()) {
        // Its much easier for other steps to get the enclosing module if we resolve now
        auto& type =
            *ctx_.pool[{type_kind::MODULE, types::mut::CONSTANT, *imported_mod->root_table_idx}];
        type.resolve<types::module>(*imported_mod);

        collecting_.set_sema_type(id, type);
        auto& sym{ctx_.registry.get_from(table_idx_, alias)};
        sym.set_status(symbol_status::RESOLVED);
        sym.set_kind(symbol_kind::MODULE);
    } else {
        ctx_.poison_symbol(ctx_.registry.get_from(table_idx_, alias));
        ctx_.poison_node(collecting_, id);
    }
}

auto symbol_collector::visit(ast::node_id id, const ast::return_stmt& return_stmt) -> void {
    PROFILE_FUNCTION();
    if (!in_function_scope_) {
        ctx_.diags.emplace_back("Cannot return outside of a function",
                                error::ILLEGAL_CONTROL_FLOW,
                                collecting_.ast.location_of(id));
    }
    if (return_stmt.expression) { collect(*return_stmt.expression); }
}

auto symbol_collector::visit(ast::node_id id, const ast::test_stmt& test) -> void {
    PROFILE_FUNCTION();
    if (table_stack_.size() != 1) {
        ctx_.diags.emplace_back("Tests must be at the topmost level of a file",
                                error::ILLEGAL_TEST_LOCATION,
                                collecting_.ast.location_of(id));
    }

    // Not a symbol so don't push to the table, track in node instead
    const auto& block{collecting_.ast.get_as<ast::block_stmt>(test.block)};
    const auto  scope_idx{visit_scopes(
        type_kind::BLOCK,
        stdx::iter_pair{.iterable = block, .visitor = [this](const ast::stmt_handle& stmt) -> void {
                            collect(stmt);
                        }})};
    last_type_->set_symbol_table_idx(scope_idx);
    collecting_.set_sema_type(id, *last_type_.take());
}

auto symbol_collector::visit(ast::node_id id, const ast::using_stmt& using_stmt) -> void {
    PROFILE_FUNCTION();
    collect(using_stmt.explicit_type);
    const auto& ident{collecting_.ast.get_as<ast::identifier_expr>(using_stmt.alias)};
    try_declare<symbols::node_t>(ident.name, id);
    ctx_.registry.get_from(table_idx_, ident.name).set_kind(symbol_kind::TYPE);
}

AST_TYPE_VISITOR_NOOP(symbol_collector, identifier_expr)
AST_TYPE_VISITOR_NOOP(symbol_collector, module_access_expr)
AST_TYPE_VISITOR_NOOP(symbol_collector, dot_expr)

auto symbol_collector::visit(ast::explicit_type_id, const ast::call_expr& call) -> void {
    PROFILE_FUNCTION();
    visit(ast::node_id::make_invalid(), call);
}

auto symbol_collector::visit(ast::explicit_type_id, const ast::explicit_function_type& fn) -> void {
    PROFILE_FUNCTION();
    for (const auto& param : fn.parameter_types) { collect(param); }
    collect(fn.explicit_return_type);
}

auto symbol_collector::visit(ast::explicit_type_id, const ast::explicit_array_type& array) -> void {
    PROFILE_FUNCTION();
    const default_counter::guard g{in_expr_scope_};
    if (array.dimension) { collect(*array.dimension); }
    collect(array.inner_explicit_type);
}

} // namespace ghoti::sema
