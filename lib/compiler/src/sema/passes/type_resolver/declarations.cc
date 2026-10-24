#include <algorithm>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/pack_element.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// When `value` is exactly `@compileError("literal")`, returns the message
[[nodiscard]] auto sole_compile_error_message(const ast::AST& ast, ast::expr_handle value)
    -> stdx::option<std::string_view> {
    const ast::node_id id{value};
    if (id.get_kind() != ast::node_kind::CALL_EXPRESSION) { return stdx::none; }
    const auto& call{ast.get_as<ast::call_expr>(id)};
    if (ast::node_id{call.function}.get_builtin() != syntax::builtin_t::COMPILE_ERROR) {
        return stdx::none;
    }
    if (call.arguments.size() != 1) { return stdx::none; }
    const auto arg{call.arguments.front().as_opt<ast::expr_handle>()};
    if (!arg) { return stdx::none; }
    const auto str{ast.get_as_opt<ast::string_expr>(*arg)};
    if (!str) { return stdx::none; }
    return str->value;
}

// A view's own mutability: `mut?`, constant, or mutable
[[nodiscard]] auto view_mutability(const type& view) noexcept
    -> stdx::option<types::mutability_modifiers> {
    switch (view.get_kind()) {
    case type_kind::POINTER:
    case type_kind::REFERENCE:
    case type_kind::SLICE:
    case type_kind::ARRAY:
        if (view.is_poly()) { return types::mut::POLY; }
        return view.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE;
    default: return stdx::none;
    }
}

} // namespace

auto type_resolver::resolve_symbol_info(ast::identifier_handle    handle,
                                        stdx::option<symbol_kind> kind) -> stdx::option<symbol&> {
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(handle)};
    return ctx_.registry.get_from_opt(table_idx_, ident.name).transform([&](symbol& sym) -> auto& {
        if (kind) { sym.set_kind(*kind); }
        sym.set_status(symbol_status::RESOLVED);
        return sym;
    });
}

namespace {

template <typename T>
concept HasName = requires(T t) { t.name; };

template <typename T>
concept HasType = requires(T t) { t.explicit_type; };

template <typename T>
concept HasNameOnly = HasName<T> && !HasType<T>;

template <typename T>
concept HasBothNameAndType = HasName<T> && HasType<T>;

} // namespace

auto type_resolver::get_resolved_symbol_type(symbol::data_t& symbol_data) -> type& {
    return symbol_data.visit(
        [](symbols::builtin& builtin) -> type& { return builtin.get_type(); },
        [this](symbols::label& label) -> type& {
            const auto defn{label.get_definition()};
            ASSERT(resolving_.has_sema_type(defn), "Resolved node has no type");
            return resolving_.get_sema_type(defn);
        },
        [this](auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym), "Directly indexable symbol was never typed");
            return resolving_.get_sema_type(sym);
        },
        [this](HasBothNameAndType auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym.name), "Symbol was never typed");
            auto& type{resolving_.get_sema_type(sym.name)};
            // An untyped pack parameter (`rest...`) has no explicit type to compare against
            const stdx::option<ast::explicit_type_id> annotation{sym.explicit_type};
            if (!annotation) { return type; }
            if (const auto explicit_opt{resolving_.get_sema_type_opt(*annotation)}) {
                const auto exp_kind{explicit_opt->get_kind()};
                ASSERT(type == *explicit_opt || exp_kind == type_kind::AUTO ||
                           exp_kind == type_kind::TYPE || is_generic_type(*explicit_opt) ||
                           is_assignable(type, *explicit_opt) || is_assignable(*explicit_opt, type),
                       "Symbol was resolved with mismatched type");
            }
            return type;
        },
        [this](HasNameOnly auto& sym) -> type& {
            ASSERT(resolving_.has_sema_type(sym.name), "Name-only sym was never typed");
            return resolving_.get_sema_type(sym.name);
        },
        [this](symbols::capture& capture) -> type& {
            ASSERT(capture.payload.is<ast::identifier_expr>(), "Capture payload must be an ident");
            ASSERT(resolving_.has_sema_type(capture.payload), "Capture was never typed");
            return resolving_.get_sema_type(capture.payload);
        });
}

auto type_resolver::is_declared_later_in_active_block(ast::node_id decl) const -> bool {
    for (const auto& frame : active_blocks_) {
        if (!frame.block) { continue; }
        const auto& statements{frame.block->statements};
        for (usize idx{frame.current_stmt_idx + 1}; idx < statements.size(); ++idx) {
            if (statements[idx].get_index() == decl.get_index()) { return true; }
        }
    }
    return false;
}

auto type_resolver::is_runtime_local_decl(ast::node_id decl) const -> bool {
    const auto stmt{resolving_.ast.get_as_opt<ast::decl_stmt>(decl)};
    if (!stmt || !stmt->value) { return stmt.has_value(); }
    if (resolving_.ast.get_as_opt<ast::function_expr>(**stmt->value)) { return false; }
    return !decl_value_denotes_type(*stmt->value);
}

template <ast::IndexableID ID> auto type_resolver::resolve_symbol(ID id, symbol& sym) -> void {
    auto& symbol_data{sym.get_data()};
    switch (sym.get_status()) {
    case symbol_status::RESOLVED: {
        // Already reported (e.g. a second use inside its own initializer)
        if (sym.has_kind() && sym.get_kind() == symbol_kind::POISONED) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }
        // Identifier handles are not unique in the tree, but their symbol can be used to find root
        // A `comptime let mut` type is whatever the statements so far left it holding
        auto& resolved{
            comptime_type_var_value(sym).value_or(get_resolved_symbol_type(symbol_data))};
        resolving_.set_sema_type(id, resolved);
        break;
    }
    case symbol_status::RESOLVING:
        if (const auto forwarded_type{forward_type(resolving_, stdx::none, sym)}) {
            resolving_.set_sema_type_if(id, *forwarded_type);
            return last_type_.emplace(*forwarded_type);
        }

        ctx_.poison_symbol(sym);
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("'{}' is used during its own resolution", sym.get_name()),
                             error::CYCLIC_DEPENDENCY,
                             resolving_.ast.location_of(id)));
    case symbol_status::UNRESOLVED: {
        sym.set_status(symbol_status::RESOLVING);

        // Fields/params/enum members can't resolve out-of-order, so early use is an ordering
        // issue, not a self-reference cycle.
        const auto node{symbol_data.as_opt<symbols::node_t>()};
        if (!node) {
            ctx_.poison_symbol(sym);
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'{}' is referenced before its declaration; forward references to "
                            "struct/union fields, function parameters, and enum members are not "
                            "supported; declare '{}' earlier",
                            sym.get_name(),
                            sym.get_name()),
                error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
                resolving_.ast.location_of(id)));
        }
        // Locals initialize in statement order, so a later one has no value yet
        if (is_declared_later_in_active_block(*node) && is_runtime_local_decl(*node)) {
            sym.set_status(symbol_status::UNRESOLVED);
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("'{}' is used before its declaration", sym.get_name()),
                                 error::ILLEGAL_FIELD_ORDER_DEPENDENCY,
                                 resolving_.ast.location_of(id)));
        }
        {
            // The declaration is typed on its own, not by the expected type at this use
            const structural_guard shield{implicit_type_stack_, stdx::none};
            resolve(*node);
        }
        // Take the symbol's real type so forward references and mutual recursion resolve.
        if (sym.get_status() == symbol_status::RESOLVED &&
            sym.get_kind() != symbol_kind::POISONED) {
            resolving_.set_sema_type(id, get_resolved_symbol_type(symbol_data));
        } else {
            resolving_.set_sema_type(id, *last_type_.take());
        }
        break;
    }
    default: UNREACHABLE("Symbol status should only be 1 of 3 states");
    }

    // A reference to a deferred-error declaration (`const X = @compileError("msg")`) reports the
    // message here, at the use site, rather than where `X` was declared.
    if (const auto msg{sym.deferred_error()}) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   std::string{*msg},
                                                   error::COMPILE_ERROR_REACHED,
                                                   resolving_.ast.location_of(id)));
    }

    if (sym.get_kind() == symbol_kind::POISONED) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    last_type_.emplace(resolving_.get_sema_type(id));
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_symbol, symbol&)

auto type_resolver::record_symbol_owner(ast::node_id         ref_id,
                                        usize                owner_table_idx,
                                        const mod::module_t& target_mod,
                                        const symbol&        sym) -> void {
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node) { return; }
    const auto decl{target_mod.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl || !decl->value) { return; }

    // A direct reference to a named function decays to a bare/scoped GIR symbol name at a call
    // or value site.
    if (target_mod.ast.get_as_opt<ast::function_expr>(*decl->value).has_value()) {
        resolving_.set_resolved_symbol_owner(ref_id, owner_table_idx);
        return;
    }

    // `const f = other.g`: follow the chain so the call site scopes to `g`'s real owning module,
    // not this alias.
    if (const auto dot{target_mod.ast.get_as_opt<ast::dot_expr>(*decl->value)}) {
        if (const auto outer_ty{target_mod.get_sema_type_opt(dot->object)}) {
            if (const auto md{outer_ty->get_data().as_opt<types::module_t>()}) {
                const auto& inner_mod{md->imported};
                if (inner_mod.root_table_idx) {
                    const auto& inner_ident{
                        target_mod.ast.get_as<ast::identifier_expr>(dot->member)};
                    if (const auto inner_sym{ctx_.registry.get_from_opt(*inner_mod.root_table_idx,
                                                                        inner_ident.name)}) {
                        record_symbol_owner(
                            ref_id, *inner_mod.root_table_idx, inner_mod, *inner_sym);
                    }
                }
            }
        }
    }
}

auto type_resolver::record_member_owner(ast::node_id           ref_id,
                                        type&                  object_type,
                                        ast::identifier_handle member) -> void {
    gsl::not_null<type*> target{&object_type};
    if (const auto meta{target->get_data().as_opt<types::meta_type>()}) {
        target = &meta->instance;
    }
    if (const auto ptr{target->get_data().as_opt<types::pointer>()}) { target = &ptr->underlying; }
    if (const auto ref{target->get_data().as_opt<types::reference>()}) {
        target = &ref->underlying;
    }
    if (const auto fn{target->get_data().as_opt<types::function>()}) { target = &fn->return_type; }

    const auto enclosing{target->get_data().visit(
        [](const types::struct_t& s) -> stdx::option<const mod::module_t&> { return s.enclosing; },
        [](const types::union_t& u) -> stdx::option<const mod::module_t&> { return u.enclosing; },
        [](const types::enum_t& e) -> stdx::option<const mod::module_t&> { return e.enclosing; },
        [](const auto&) -> stdx::option<const mod::module_t&> { return stdx::none; })};
    if (!enclosing || !target->has_symbol_table_idx()) { return; }

    const auto  table_idx{target->get_symbol_table_idx()};
    const auto& member_ident{resolving_.ast.get_as<ast::identifier_expr>(member)};
    if (const auto sym{ctx_.registry.get_from_opt(table_idx, member_ident.name)}) {
        record_symbol_owner(ref_id, table_idx, *enclosing, *sym);
        // Covers `.field = v` initializers and `.member` implicit access, which record no
        // declaration reference of their own
        report_deprecated_use(ast::node_id{member}, *enclosing, *sym);
    }
}

template <ast::IndexableID ID>
auto type_resolver::resolve_ident(ID id, const ast::identifier_expr& ident) -> void {
    const auto name{ident.name};

    // A pack is usable only as `.len`, `[K]`, `unroll for`'s iterable, or `expr...`. Those
    // forms intercept before ever reaching here, so a bare mention of the name is out of position.
    if (pack_named(name)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("'{}' is a parameter pack; use '{}.len', '{}[k]', "
                                         "'{}...', or an `unroll for` iterable",
                                         name,
                                         name,
                                         name,
                                         name),
                             error::PACK_USE_OUT_OF_POSITION,
                             resolving_.ast.location_of(id)));
    }

    // A primitive keyword names the primitive even where a raw identifier (`@"f128"`) declares
    // the same name. The raw identifier is an ordinary name
    const auto tok{id.get_token_type()};
    const bool is_primitive_token{std::ranges::contains(syntax::ALL_PRIMITIVES, tok)};

    // `iN` / `uN` are primitive integer types, not looked-up symbols.
    if (syntax::token_type::is_int_type_lexeme(name) &&
        (tok == syntax::token_type_t::INT_TYPE ||
         !ctx_.registry.lookup_with_depth(table_stack_, name))) {
        u64 width{0};
        for (const char c : stdx::string::substr(name, 1)) {
            width = width * 10 + static_cast<u64>(c - '0');
            if (width > 65'535) { break; }
        }
        if (width > 65'535) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Integer width must be at most 65535; found '{}'", name),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(id)));
        }
        auto& int_type{ctx_.get_int(static_cast<u16>(width), name.front() == 'i')};
        resolving_.set_sema_type(id, int_type);
        return last_type_.emplace(int_type);
    }

    symbol_table_stack prelude_only;
    const bool         shadowed_primitive{[&] {
        if (!is_primitive_token) { return false; }
        const auto found{ctx_.registry.lookup_with_depth(table_stack_, name)};
        return found && found->depth != 0;
    }()};
    if (shadowed_primitive) { prelude_only.push(*ctx_.prelude_index); }
    auto lookup{
        ctx_.registry.lookup_with_depth(shadowed_primitive ? prelude_only : table_stack_, name)};

    // Check for an undeclared identifier and poison the ident
    if (!lookup) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Use of undeclared identifier '{}'", name),
                             error::UNDECLARED_IDENTIFIER,
                             resolving_.ast.location_of(id)));
    }

    // Inside a method a sibling field is only reachable through an instance
    if constexpr (std::same_as<ID, ast::node_id>) {
        const auto& data{lookup->symbol.get_data()};
        const bool  is_field{data.template is<symbols::struct_field>() ||
                             data.template is<symbols::union_field>()};
        if (is_field && !return_trackers_.empty()) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("'{0}' is a field; access it through an instance, "
                                             "e.g. `self.{0}`",
                                             name),
                                 error::UNDECLARED_IDENTIFIER,
                                 resolving_.ast.location_of(id)));
        }
    }

    // Record where this reference resolves to, for LSP go-to-definition
    resolving_.set_identifier_definition(
        id, {resolving_.path, lookup->symbol.get_symbol_span(resolving_)});
    if (const auto declared_in{ctx_.registry.lookup_with_table(table_stack_, name)}) {
        record_scoped_declaration(id, declared_in->table_idx, lookup->symbol);
    }
    if constexpr (std::same_as<ID, ast::node_id>) { resolving_.add_identifier_position(id); }

    // A local generic's monomorph runs apart from the frame that wrote it, so whatever it uses from
    // that frame is captured into an environment built where the generic is written
    const bool captured_by_generic{
        enclosing_locals_depth_ && instantiation_root_depth_ && instantiating_fn_node_ &&
        lookup->depth >= *enclosing_locals_depth_ && lookup->depth < *instantiation_root_depth_};
    // Belongs to an enclosing function's stack frame rather than the module/prelude scope
    const bool captured_by_open_fn{!function_boundaries_.empty() &&
                                   lookup->depth < function_boundaries_.back() &&
                                   lookup->depth >= outermost_function_boundary()};
    if (captured_by_generic || captured_by_open_fn) {
        const auto self_id{open_function_nodes_.empty() ? *instantiating_fn_node_
                                                        : open_function_nodes_.back()};
        // A function referring to its own name is recursion, not a captured free variable
        bool                       is_self_reference{false};
        stdx::option<ast::node_id> captured_decl;
        if (const auto node{lookup->symbol.get_data().as_opt<symbols::node_t>()}) {
            if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
                captured_decl.emplace(*node);
                if (decl->value) {
                    const ast::node_id value_id{*decl->value};
                    is_self_reference = value_id.get_kind() == self_id.get_kind() &&
                                        value_id.get_index() == self_id.get_index();
                }
            }
        }

        if (is_self_reference) {
            if (!self_recursive_flags_.empty()) { self_recursive_flags_.back() = true; }
        } else if (captured_by_generic && captured_decl &&
                   is_written_after(*captured_decl, *instantiating_fn_node_) &&
                   !is_compile_time_only(lookup->symbol)) {
            // Its environment is built where the generic is written, before this binding exists
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'{}' is declared after the generic local function that captures it",
                            name),
                error::GENERIC_CLOSURE_CAPTURE,
                resolving_.ast.location_of(id)));
        } else if (!is_compile_time_only(lookup->symbol)) {
            auto usage{in_mutating_context_ ? capture_usage::MUTATED : capture_usage::READ};

            // A closure needs a mutable binding to write through, but a `move fn` owns a copy.
            // Writing through an immutable view only reads the binding
            const auto closure{resolving_.ast.get_as_opt<ast::function_expr>(self_id)};
            const bool immutable{!is_mutable_local(lookup->symbol)};
            // A binding declared later has no type yet. Using it is reported on its own
            if (usage == capture_usage::MUTATED && immutable &&
                lookup->symbol.get_status() == symbol_status::RESOLVED) {
                const auto kind{get_resolved_symbol_type(lookup->symbol.get_data()).get_kind()};
                if (kind == type_kind::SLICE || kind == type_kind::POINTER ||
                    kind == type_kind::REFERENCE) {
                    usage = capture_usage::READ;
                }
            }
            if (usage == capture_usage::MUTATED && !(closure && closure->is_move) && immutable) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("A closure can only modify a captured 'let mut' binding, and '{}' "
                                "is not one",
                                name),
                    error::ASSIGNMENT_TO_CONST,
                    resolving_.ast.location_of(id)));
            }

            // Find the innermost open function whose own scope actually contains the declaration.
            // A generic body being instantiated owns it when no open function does
            usize first_capturing{0};
            for (usize i{function_boundaries_.size()}; i-- > 0;) {
                if (lookup->depth >= function_boundaries_[i]) {
                    first_capturing = i + 1;
                    break;
                }
            }
            if (captured_by_generic) {
                resolving_.add_capture(*instantiating_fn_node_, name, usage);
                first_capturing = 0;
            }
            for (usize i{first_capturing}; i < open_function_nodes_.size(); ++i) {
                resolving_.add_capture(open_function_nodes_[i], name, usage);
            }
        }
    }

    if constexpr (std::same_as<ID, ast::node_id>) {
        if (const auto located{ctx_.registry.lookup_with_table(table_stack_, name)}) {
            resolving_.set_symbol_table(id, located->table_idx);
            record_symbol_owner(id, located->table_idx, resolving_, located->symbol);
        }
    }

    // A declaration outside every open function resolves on first use as if from its own scope.
    // otherwise its functions would see the open ones' frames and capture from them
    if (lookup->symbol.get_status() == symbol_status::UNRESOLVED && !function_boundaries_.empty() &&
        lookup->depth < outermost_function_boundary()) {
        const std::vector<usize> outer_tables(table_stack_.begin(), table_stack_.end());
        for (usize i{lookup->depth + 1}; i < outer_tables.size(); ++i) { table_stack_.pop(); }
        const auto outer_table_idx{std::exchange(table_idx_, outer_tables[lookup->depth])};
        auto       outer_boundaries{std::exchange(function_boundaries_, {})};
        auto       outer_fn_nodes{std::exchange(open_function_nodes_, {})};
        auto       outer_recursive{std::exchange(self_recursive_flags_, {})};
        const auto restore{gsl::finally([&] {
            for (usize i{lookup->depth + 1}; i < outer_tables.size(); ++i) {
                table_stack_.push(outer_tables[i]);
            }
            table_idx_            = outer_table_idx;
            function_boundaries_  = std::move(outer_boundaries);
            open_function_nodes_  = std::move(outer_fn_nodes);
            self_recursive_flags_ = std::move(outer_recursive);
        })};
        resolve_symbol(id, lookup->symbol);
        return;
    }

    resolve_symbol(id, lookup->symbol);
}

VISITOR_TEMPLATE_INIT(type_resolver, resolve_ident, const ast::identifier_expr&)

auto type_resolver::visit(ast::node_id id, const ast::identifier_expr& ident) -> void {
    PROFILE_FUNCTION();
    // A builtin has no value of its own. Only a call to it means anything
    const bool is_callee{std::exchange(resolving_callee_, false)};
    if (!is_callee && id.get_token_type() == syntax::token_type_t::BUILTIN) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Builtin '{}' must be called", ident.name),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }
    resolve_ident(id, ident);
    if (!is_callee && last_type_ && last_type_->is_generic_value()) {
        instantiate_for_expected_callable(id, *last_type_);
    }

    // A range counter reads as any integer type that holds every value it takes
    if (const auto expected{implicit_type_stack_.peek()};
        expected && last_type_ && is_integer(last_type_->get_kind()) &&
        !is_same_unqualified(*last_type_, *expected) && adopt_counter_type(id, *expected)) {
        last_type_.emplace(resolving_.get_sema_type(id));
    }
}

auto type_resolver::decl_value_denotes_type(ast::expr_handle value) const -> bool {
    if (value.any<ast::struct_expr, ast::union_expr, ast::enum_expr, ast::interface_expr>()) {
        return true;
    }
    if (is_type_denoting_expr(ctx_, resolving_, value, &table_stack_)) { return true; }
    if (type_valued_results_.contains(ast::node_id{value}.get_index())) { return true; }

    // `Ctor(T)` / `@Int(...)`: a call to anything whose declared result is `type`
    if (const auto call{resolving_.ast.get_as_opt<ast::call_expr>(value)}) {
        const auto callee{resolving_.get_sema_type_opt(call->function)};
        if (!callee) { return false; }
        // A generic's declared return may be a `T: type` parameter standing in for a value type,
        // so only a literal `: type` annotation marks a constructor
        if (const auto generic{ctx_.generic_functions.get_opt(*callee)}) {
            return generic->fn_expr->explicit_return_type.get_token_type() ==
                   syntax::token_type_t::TYPE_TYPE;
        }
        if (const auto fn{callee->get_data().as_opt<types::function>()}) {
            // A `[n]T` return is a (not yet folded) array value, not a type
            return fn->return_type.get_kind() == type_kind::TYPE &&
                   !fn->return_type.get_data().is<types::deferred_array>();
        }
        if (const auto builtin{callee->get_data().as_opt<types::builtin_function>()}) {
            return builtin->return_type.get_kind() == type_kind::TYPE;
        }
        return false;
    }

    // `mod.Type` / `Outer.Inner`
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(value)}) {
        const auto sym{dot_member_symbol(*dot)};
        return sym && sym->has_kind() && sym->get_kind() == symbol_kind::TYPE;
    }

    // `^mut u8` / `&T` over a type is the pointer or reference type
    if (const auto addr{resolving_.ast.get_as_opt<ast::address_of_expr>(value)}) {
        return operand_nature(addr->rhs) == operand_nature_t::TYPE;
    }
    if (const auto ref{resolving_.ast.get_as_opt<ast::reference_expr>(value)}) {
        return operand_nature(ref->rhs) == operand_nature_t::TYPE;
    }
    return false;
}

template <ast::IndexableID ID>
auto type_resolver::record_scoped_declaration(ID id, usize table_idx, const symbol& sym) -> void {
    // A bare name is declared in this module, or in the prelude's own module
    if (!ctx_.prelude_index || table_idx != *ctx_.prelude_index) {
        return record_declaration(id, resolving_, sym);
    }
    if (ctx_.modules.has_builtin_module()) {
        record_declaration(id, ctx_.modules.builtin_module(), sym);
    }
}

template auto
type_resolver::record_scoped_declaration<ast::node_id>(ast::node_id, usize, const symbol&) -> void;
template auto type_resolver::record_scoped_declaration<ast::explicit_type_id>(ast::explicit_type_id,
                                                                              usize,
                                                                              const symbol&)
    -> void;

template <ast::IndexableID ID>
auto type_resolver::record_declaration(ID id, mod::module_t& owner, const symbol& sym) -> void {
    const auto ref{sym.get_data().visit(
        [&](const symbols::node_t& node) -> stdx::option<declaration_ref> {
            if (!owner.ast.get_as_opt<ast::decl_stmt>(node)) { return stdx::none; }
            return declaration_ref{
                .owner      = owner,
                .decl       = ast::node_id{node},
                .annotation = stdx::none,
            };
        },
        [&](const symbols::parameter& param) -> stdx::option<declaration_ref> {
            if (!param.explicit_type) { return stdx::none; }
            return declaration_ref{
                .owner      = owner,
                .decl       = stdx::none,
                .annotation = param.explicit_type,
            };
        },
        [&](const symbols::struct_field& field) -> stdx::option<declaration_ref> {
            return declaration_ref{
                .owner      = owner,
                .decl       = stdx::none,
                .annotation = field.explicit_type,
            };
        },
        [&](const symbols::union_field& field) -> stdx::option<declaration_ref> {
            return declaration_ref{
                .owner      = owner,
                .decl       = stdx::none,
                .annotation = field.explicit_type,
            };
        },
        [](const auto&) -> stdx::option<declaration_ref> { return stdx::none; })};
    if (ref) { resolving_.set_identifier_declaration(id, *ref); }
    report_deprecated_use(id, owner, sym);
    check_testing_use(id, owner, sym);
}

template auto type_resolver::record_declaration<ast::node_id>(ast::node_id,
                                                              mod::module_t&,
                                                              const symbol&) -> void;
template auto type_resolver::record_declaration<ast::explicit_type_id>(ast::explicit_type_id,
                                                                       mod::module_t&,
                                                                       const symbol&) -> void;

auto type_resolver::thin_if_comptime(const ast::function_expr::parameter& param, type& param_type)
    -> type& {
    if (!param.is_comptime || !param.explicit_type) { return param_type; }
    auto& thin{ctx_.with_erasure(param_type, false)};
    if (&thin != &param_type) { resolving_.set_sema_type(*param.explicit_type, thin); }
    return thin;
}

auto type_resolver::holds_undefined_by_value(const type& t) -> bool {
    if (t.get_kind() == type_kind::UNDEFINED) { return true; }
    if (const auto arr{t.get_data().as_opt<types::array>()}) {
        return holds_undefined_by_value(arr->underlying);
    }
    if (const auto deferred{t.get_data().as_opt<types::deferred_array>()}) {
        return holds_undefined_by_value(deferred->underlying);
    }
    return false;
}

auto type_resolver::reject_non_runtime_slot(ast::explicit_type_id at, const type& slot_type)
    -> bool {
    if (reject_unsized_slot(at, slot_type)) { return true; }
    if (holds_undefined_by_value(slot_type)) {
        ctx_.diags.emplace_back("'@TypeOf(undefined)' only exists at compile time; only a 'const' "
                                "binding can have this type",
                                error::COMPILE_TIME_ONLY_VALUE,
                                resolving_.ast.location_of(at));
        return true;
    }
    return false;
}

auto type_resolver::reject_signature_slot(ast::explicit_type_id at, const type& slot_type) -> bool {
    if (reject_non_runtime_slot(at, slot_type)) { return true; }
    if (slot_type.get_kind() == type_kind::OPAQUE) {
        ctx_.diags.emplace_back(opaque_by_value_error(resolving_.ast.location_of(at)));
        return true;
    }
    return false;
}

auto type_resolver::reject_storage_slot(ast::explicit_type_id at, const type& slot_type) -> bool {
    if (reject_non_runtime_slot(at, slot_type)) { return true; }
    // Only a return or a diverging expression has this type. Nothing can be stored as it
    if (slot_type.get_kind() == type_kind::NORETURN) {
        ctx_.diags.emplace_back("'noreturn' has no values, so nothing can be stored with this type",
                                error::TYPE_MISMATCH,
                                resolving_.ast.location_of(at));
        return true;
    }
    return false;
}

auto type_resolver::reject_unsized_slot(ast::explicit_type_id at, const type& slot_type) -> bool {
    if (slot_type.get_kind() == type_kind::INTERFACE) {
        const auto iname{ctx_.type_display_name(slot_type)};
        ctx_.diags.emplace_back(
            fmt::format("`{0}` is an interface and cannot be stored by value; use `&dyn {0}`, "
                        "`^dyn {0}`, or an `impl {0}` parameter",
                        iname),
            error::INTERFACE_NOT_A_VALUE,
            resolving_.ast.location_of(at));
        return true;
    }
    if (slot_type.get_kind() == type_kind::DYN) {
        const auto dname{ctx_.type_display_name(slot_type)};
        ctx_.diags.emplace_back(fmt::format("`{0}` is unsized; use `&{0}` or `^{0}`", dname),
                                error::ILLEGAL_UNSIZED_TYPE,
                                resolving_.ast.location_of(at));
        return true;
    }
    return false;
}

auto type_resolver::reject_unassignable_global_initializer(ast::expr_handle value,
                                                           const type&      declared,
                                                           bool             arrays_only) -> bool {
    const auto value_type{resolving_.get_sema_type_opt(value)};
    if (!value_type || value_type->is_poison() || !declared.is_resolved() || declared.is_poison()) {
        return false;
    }
    // Placeholders and untyped literals settle against the annotation later
    // Array initializers are copied and coerced element-wise when the global is folded
    const auto is_array_like{[](const type& t) {
        return t.get_data().is<types::array>() || t.get_data().is<types::deferred_array>();
    }};
    const auto declared_kind{declared.get_kind()};
    const bool array_target{is_array_like(declared) || declared_kind == type_kind::SLICE ||
                            declared_kind == type_kind::POINTER ||
                            declared_kind == type_kind::REFERENCE};
    if (is_array_like(declared) && value_type->get_kind() == type_kind::UNDEFINED) { return false; }
    // Arrays fold element by element, so their elements are what has to fit
    if (is_array_like(*value_type) && array_target) {
        auto&      sized_value{concrete_array_type(*value_type)};
        auto&      sized_declared{concrete_array_type(const_cast<type&>(declared))};
        const bool fits{sized_value.get_data().is<types::deferred_array>() ||
                        sized_declared.get_data().is<types::deferred_array>() ||
                        declared_kind == type_kind::POINTER ||
                        declared_kind == type_kind::REFERENCE ||
                        fills_by_value(sized_value, sized_declared)};
        if (fits) { return false; }
        last_type_.emplace(ctx_.poison_node(
            resolving_,
            value,
            ctx_.store_mismatch_message(sized_value, sized_declared, target_ptr_bits()),
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(value)));
        return true;
    }
    // A local `const`'s scalar initializer is checked where it is stored
    if (arrays_only) { return false; }
    if (const auto slice{value_type->get_data().as_opt<types::slice>()};
        slice && is_comptime_numeric(slice->underlying.get_kind())) {
        return false;
    }
    // An untyped numeric literal is range-checked against a numeric annotation when emitted
    const bool literal_into_numeric{
        is_comptime_numeric(value_type->get_kind()) &&
        (is_integer(declared.get_kind()) || is_float(declared.get_kind()))};
    if (is_generic_type(*value_type, false) || is_generic_type(declared, false) ||
        literal_into_numeric || is_assignable(*value_type, declared)) {
        return false;
    }
    last_type_.emplace(
        ctx_.poison_node(resolving_,
                         value,
                         ctx_.store_mismatch_message(*value_type, declared, target_ptr_bits()),
                         error::TYPE_MISMATCH,
                         resolving_.ast.location_of(value)));
    return true;
}

auto type_resolver::frame_local_root(ast::expr_handle returned) -> stdx::option<std::string_view> {
    if (return_trackers_.empty()) { return stdx::none; }
    stdx::option<ast::node_id> place;
    if (const auto ref{resolving_.ast.get_as_opt<ast::reference_expr>(returned)}) {
        place.emplace(*ref->rhs);
    } else if (const auto addr{resolving_.ast.get_as_opt<ast::address_of_expr>(returned)}) {
        place.emplace(*addr->rhs);
    }
    if (!place) { return stdx::none; }

    // Fields and array elements live in their object. A slice, pointer, or reference doesn't
    const auto is_indirect{[&](ast::node_id n) {
        const auto t{resolving_.get_sema_type_opt(n)};
        if (!t) { return true; }
        const auto k{t->get_kind()};
        return k == type_kind::POINTER || k == type_kind::REFERENCE || k == type_kind::SLICE;
    }};
    while (true) {
        if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(*place)}) {
            if (is_indirect(dot->object)) { return stdx::none; }
            place.emplace(dot->object);
        } else if (const auto index{resolving_.ast.get_as_opt<ast::index_expr>(*place)}) {
            if (is_indirect(index->array)) { return stdx::none; }
            place.emplace(index->array);
        } else {
            break;
        }
    }

    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(*place)};
    if (!ident || is_indirect(*place)) { return stdx::none; }
    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
    if (!sym) { return stdx::none; }
    const bool is_local{sym->get_data().visit(
        [](const symbols::parameter& p) { return !p.is_comptime; },
        [](const symbols::self_parameter& s) { return s.modifier.is_value(); },
        [&](const symbols::node_t& node) {
            // A `const` folds into read-only storage of its own. Only runtime locals die here
            const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(node)};
            if (!decl || !decl->has_modifier(ast::decl_modifiers::LET) ||
                decl->has_modifier(ast::decl_modifiers::COMPTIME)) {
                return false;
            }
            const auto root{resolving_.root_table_idx};
            const auto global{root ? ctx_.registry.get_from_opt(*root, ident->name) : stdx::none};
            return !global || global.get() != sym.get();
        },
        [](const auto&) { return false; })};
    if (!is_local) { return stdx::none; }
    return ident->name;
}

auto type_resolver::reject_type_condition(ast::expr_handle condition) -> bool {
    if (operand_nature(condition) != operand_nature_t::TYPE) { return false; }
    last_type_.emplace(ctx_.poison_node(
        resolving_,
        condition,
        fmt::format("A condition must be a value, but '{}' is a type",
                    ctx_.type_display_name(denoted_type(resolving_.get_sema_type(condition)))),
        error::TYPE_USED_AS_VALUE,
        resolving_.ast.location_of(condition)));
    return true;
}

auto type_resolver::reject_loop_as_value(ast::expr_handle value) -> bool {
    // Only a labeled loop yields a value, through `break :label v`
    const bool is_loop{value.is<ast::while_loop_expr>() || value.is<ast::for_loop_expr>() ||
                       value.is<ast::infinite_loop_expr>() || value.is<ast::do_while_loop_expr>()};
    if (!is_loop) { return false; }
    last_type_.emplace(ctx_.poison_node(
        resolving_,
        value,
        "A loop without a label has no value; label it and leave it with `break :label value`",
        error::TYPE_MISMATCH,
        resolving_.ast.location_of(value)));
    return true;
}

auto type_resolver::reject_type_as_value(ast::expr_handle value, const type& expected) -> bool {
    // A `type`, `auto`, or bare generic slot legitimately takes a type, and a not-yet-folded
    // `[N]T` slot shares the `type` kind. A generic `[]T`, `^T`, or `&T` slot is still a value.
    const auto kind{expected.get_kind()};
    const bool value_shaped{kind == type_kind::SLICE || kind == type_kind::POINTER ||
                            kind == type_kind::REFERENCE};
    if (!expected.is_resolved() || expected.is_poison() ||
        (is_generic_type(expected) && !value_shaped) || kind == type_kind::TYPE) {
        return false;
    }
    const auto value_type{resolving_.get_sema_type_opt(value)};
    if (!value_type || value_type->is_poison()) { return false; }
    // A bare `type` (or a generic template's placeholder) can't say whether it names a type yet,
    // but the `type` keyword itself always does
    if (value_type->get_kind() == type_kind::TYPE &&
        ast::node_id{value}.get_token_type() != syntax::token_type_t::TYPE_TYPE &&
        !value_type->get_data().is<types::meta_type>() &&
        !value_type->get_data().is<types::deferred_call>()) {
        return false;
    }
    if (!decl_value_denotes_type(value)) { return false; }

    last_type_.emplace(
        ctx_.poison_node(resolving_,
                         value,
                         fmt::format("Expected a value of type '{}', but found the type '{}'",
                                     ctx_.type_display_name(expected),
                                     ctx_.type_display_name(denoted_type(*value_type))),
                         error::TYPE_USED_AS_VALUE,
                         resolving_.ast.location_of(value)));
    return true;
}

auto type_resolver::place_mutability(ast::node_id expr) -> types::mutability_modifiers {
    const auto type{resolving_.get_sema_type_opt(expr)};
    // A reference stands for its referent
    if (type && type->get_kind() == type_kind::REFERENCE) { return *view_mutability(*type); }

    const auto through{[&](ast::node_id object) -> types::mutability_modifiers {
        const auto object_type{resolving_.get_sema_type_opt(object)};
        if (object_type && object_type->get_kind() != type_kind::ARRAY) {
            if (const auto m{view_mutability(*object_type)}) { return *m; }
        }
        // An array's `mut` / `mut?` elements are writable on their own. Otherwise its binding
        // decides
        if (object_type && object_type->get_kind() == type_kind::ARRAY) {
            if (object_type->is_poly()) { return types::mut::POLY; }
            if (!object_type->is_constant()) { return types::mut::MUTABLE; }
        }
        return place_mutability(object);
    }};

    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)}) {
        const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
        const auto node{sym ? sym->get_data().as_opt<symbols::node_t>() : stdx::none};
        const auto decl{node ? resolving_.ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
        if (decl && decl->has_modifier(ast::decl_modifiers::MUT) &&
            !decl->has_modifier(ast::decl_modifiers::COMPTIME)) {
            return types::mut::MUTABLE;
        }
        return types::mut::CONSTANT;
    }
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(expr)}) {
        return through(dot->object);
    }
    if (const auto idx{resolving_.ast.get_as_opt<ast::index_expr>(expr)}) {
        return through(idx->array);
    }
    if (const auto deref{resolving_.ast.get_as_opt<ast::dereference_expr>(expr)}) {
        if (const auto rhs_type{resolving_.get_sema_type_opt(deref->rhs)}) {
            if (const auto m{view_mutability(*rhs_type)}) { return *m; }
        }
    }
    return types::mut::CONSTANT;
}

auto type_resolver::call_mutability(const ast::call_expr&           call,
                                    const types::function&          fn,
                                    bool                            implicit_self,
                                    gsl::span<const usize>          source_index,
                                    gsl::span<const stdx::opt_size> pack_k)
    -> types::mutability_modifiers {
    auto       mutability{types::mut::MUTABLE};
    const auto combine{[&](types::mutability_modifiers arg) {
        if (mutability == types::mut::CONSTANT || arg == types::mut::CONSTANT) {
            mutability = types::mut::CONSTANT;
        } else if (arg == types::mut::POLY) {
            mutability = types::mut::POLY;
        }
    }};
    // A view argument has its own mutability. An array (or a receiver) is the place passed
    const auto arg_mutability{[&](ast::node_id arg) -> types::mutability_modifiers {
        const auto arg_type{resolving_.get_sema_type_opt(arg)};
        if (arg_type && arg_type->get_kind() != type_kind::ARRAY) {
            if (const auto m{view_mutability(*arg_type)}) { return *m; }
        }
        // An array's `mut` / `mut?` elements are writable on their own. Otherwise its binding
        // decides
        if (arg_type && arg_type->get_kind() == type_kind::ARRAY &&
            (arg_type->is_poly() || !arg_type->is_constant())) {
            return *view_mutability(*arg_type);
        }
        return place_mutability(arg);
    }};

    for (usize p{0}; p < fn.params.size(); ++p) {
        if (!fn.params[p]->is_poly()) { continue; }
        if (implicit_self && p == 0) {
            if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)}) {
                combine(arg_mutability(dot->object));
            }
            continue;
        }
        const usize slot{p - (implicit_self ? 1UZ : 0UZ)};
        if (slot >= source_index.size() || (slot < pack_k.size() && pack_k[slot])) { continue; }
        if (const auto arg{call.arguments[source_index[slot]].as_opt<ast::expr_handle>()}) {
            combine(arg_mutability(*arg));
        }
    }
    return mutability;
}

auto type_resolver::substitute_poly(type& t, types::mutability_modifiers m) -> type& {
    if (!contains_poly(t)) { return t; }
    const auto  own{t.is_poly() ? m : t.get_key().get_mut()};
    const auto& data{t.get_data()};
    if (const auto p{data.as_opt<types::pointer>()}) {
        return ctx_.get_pointer(own, substitute_poly(p->underlying, m));
    }
    if (const auto r{data.as_opt<types::reference>()}) {
        return ctx_.get_reference(own, substitute_poly(r->underlying, m));
    }
    if (const auto sl{data.as_opt<types::slice>()}) {
        return ctx_.get_slice(own, sl->null_terminated, substitute_poly(sl->underlying, m));
    }
    if (const auto ar{data.as_opt<types::array>()}) {
        return ctx_.get_array(
            own, ar->null_terminated, ar->len, substitute_poly(ar->underlying, m));
    }
    return t;
}

auto type_resolver::reject_poly_storage(ast::explicit_type_id at) -> bool {
    if (!spells_poly(resolving_.ast, at)) { return false; }
    ctx_.diags.emplace_back("`mut?` takes its mutability from a function's `mut?` parameter, so "
                            "only a function's parameters, result, and locals can use it",
                            error::ILLEGAL_POLY_MUTABILITY,
                            resolving_.ast.location_of(at));
    return true;
}

auto type_resolver::reject_unrooted_poly(ast::node_id id, ast::expr_handle operand)
    -> stdx::option<type&> {
    const auto token{id.get_token_type()};
    if (token != syntax::token_type_t::AND_MUT_POLY &&
        token != syntax::token_type_t::CARET_MUT_POLY) {
        return stdx::none;
    }
    // `&mut? T` in a type position names a type, not a place
    if (is_type_denoting_expr(ctx_, resolving_, operand, &table_stack_)) { return stdx::none; }
    if (place_mutability(operand) == types::mut::POLY) { return stdx::none; }
    return ctx_.poison_node(
        resolving_,
        id,
        fmt::format("`{}` needs a place reached through a `mut?` parameter, which is where its "
                    "mutability comes from",
                    token == syntax::token_type_t::AND_MUT_POLY ? "&mut?" : "^mut?"),
        error::ILLEGAL_POLY_MUTABILITY,
        resolving_.ast.location_of(id));
}

namespace {

[[nodiscard]] auto cabi_nonabi_scalar(std::string_view type_name, const source_location& location)
    -> diagnostic {
    return diagnostic{
        fmt::format("'{}' has no C ABI representation; extern signatures accept 8/16/32/64-bit "
                    "integers, usize/isize, bool, f32, f64, and f80 on x86",
                    type_name),
        error::ILLEGAL_REFERENCE_FIELD,
        location};
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::decl_stmt& decl) -> void {
    PROFILE_FUNCTION();
    const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(*decl.name)};
    auto        symbol_opt{ctx_.registry.lookup(table_stack_, ident.name)};
    ASSERT(symbol_opt, "Somehow the declaration was lost in the symbol table");
    const bool is_deprecated{decl.attributes &&
                             decl.attributes->find(ast::attribute_kind::DEPRECATED)};
    if (is_deprecated) { ++deprecated_scope_depth_; }
    const auto                      deprecated_scope_restore{gsl::finally([&] {
        if (is_deprecated) { --deprecated_scope_depth_; }
    })};
    const comptime_evaluation_scope cx_scope{ctx_, decl.evaluates_at_compile_time()};
    auto&                           sym{*symbol_opt};

    // Ensure malformed symbols don't crash the compiler
    if (const auto owner{sym.get_data().as_opt<symbols::node_t>()};
        !owner || owner->get_kind() != id.get_kind() || owner->get_index() != id.get_index()) {
        resolving_.set_sema_type(decl.name, ctx_.get_poison());
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    // An exported symbol is one concrete C-ABI function. A generic has no single body to export
    if (decl.has_modifier(ast::decl_modifiers::EXPORT) && decl.value) {
        if (const auto fn_expr{resolving_.ast.get_as_opt<ast::function_expr>(*decl.value)};
            fn_expr && declares_generic_params(*fn_expr)) {
            ctx_.poison_symbol(sym);
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("'{}' cannot be exported because it is generic", ident.name),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(id)));
        }
    }

    // An instantiation resolver re-types a body an earlier monomorphization resolved, so its
    // body-local decls must resolve again and overwrite their node types
    const bool value_is_deferred_generic_method{[&] {
        if (!decl.value) { return false; }
        const auto fn_expr{resolving_.ast.get_as_opt<ast::function_expr>(*decl.value)};
        if (!fn_expr || !fn_expr->self) { return false; }
        const auto existing{resolving_.get_sema_type_opt(*decl.value)};
        return existing && existing->is_resolved() &&
               ctx_.generic_functions.get_opt(*existing).has_value();
    }()};
    // An explicit annotation is re-resolved too, since it may name a generic parameter
    // (`let mut a: T`, `let mut a: @TypeOf(value)`, a type alias built from either)
    const bool reresolve_local{
        !value_is_deferred_generic_method && for_generic_instantiation_ && reresolve_floor_ && [&] {
            const auto lt{ctx_.registry.lookup_with_table(table_stack_, ident.name)};
            return lt && lt->table_idx >= *reresolve_floor_;
        }()};

    // Breaking out early is possible due to out of order semantics
    if (sym.get_status() == symbol_status::RESOLVED && !reresolve_local) {
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        // `id` may not be the node that resolved this symbol
        if (!resolving_.has_sema_type(id)) {
            resolving_.set_sema_type(decl.name, void_type);
            resolving_.set_sema_type(id, void_type);
        }
        return last_type_.emplace(void_type);
    }
    sym.set_status(symbol_status::RESOLVING);

    // `const X = @compileError("msg")` is inert until referenced. Record the message on the
    // symbol and resolve it to `void`; `resolve_symbol` reports `msg` at each use site.
    if (decl.value) {
        if (const auto msg{sole_compile_error_message(resolving_.ast, *decl.value)}) {
            sym.set_deferred_error(std::string{*msg});
            sym.set_status(symbol_status::RESOLVED);
            sym.set_kind(symbol_kind::VALUE);
            auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
            resolving_.set_sema_type(decl.name, void_type);
            resolving_.set_sema_type(id, void_type);
            return last_type_.emplace(void_type);
        }
    }

    const auto poison_out = [&] -> void {
        resolving_.set_sema_type(decl.name, *last_type_);
        ctx_.poison_symbol(sym);
        resolving_.set_sema_type(id, *last_type_);
    };

    {
        stdx::option<structural_guard> type_guard;
        bool                           annotation_typed_decl{false};

        // With an explicit type, the ident should always adopt that exact type
        if (decl.explicit_type) {
            resolve(*decl.explicit_type);
            if (last_type_->is_poison()) { return poison_out(); }
            gsl::not_null explicit_type_p{&denoted_type(*last_type_.take())};
            // `const x: MakesAType() = ...`
            if (explicit_type_p->get_data().is<types::deferred_call>()) {
                const auto& dc_call{explicit_type_p->get_data().as<types::deferred_call>().call};
                gir::const_eval evaluator{ctx_, resolving_};
                explicit_type_p = &denoted_type(evaluator.force_deferred_call(*explicit_type_p));
                register_non_generic_type_ctor_members(*explicit_type_p, dc_call);
            }
            // A C-ABI symbol has no erased callables, so its `fn(...)` is the thin pointer
            if (decl.has_modifier(ast::decl_modifiers::EXTERN) ||
                decl.has_modifier(ast::decl_modifiers::EXPORT)) {
                explicit_type_p = &ctx_.with_erasure(*explicit_type_p, false);
                resolving_.set_sema_type(*decl.explicit_type, *explicit_type_p);
            }
            auto&      explicit_type{*explicit_type_p};
            const bool aliases_undefined{explicit_type.get_kind() == type_kind::UNDEFINED};
            const bool global{function_boundaries_.empty()};
            if ((aliases_undefined ? reject_unsized_slot(*decl.explicit_type, explicit_type)
                                   : reject_storage_slot(*decl.explicit_type, explicit_type)) ||
                (global && reject_poly_storage(*decl.explicit_type))) {
                ctx_.poison_symbol(sym);
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
            if (explicit_type.get_kind() == type_kind::AUTO) {
                // `undefined` carries no type, so it cannot drive `auto` inference either.
                const bool undef_init{decl.value &&
                                      resolving_.ast.get_as_opt<ast::undefined_expr>(*decl.value)};
                if (!decl.value || undef_init) {
                    ctx_.poison_symbol(sym,
                                       "Type 'auto' requires an initializer expression",
                                       error::AUTO_WITHOUT_INITIALIZER,
                                       resolving_.ast.location_of(id));
                    resolving_.set_sema_type(decl.name, ctx_.get_poison());
                    return last_type_.emplace(ctx_.poison_node(resolving_, id));
                }
            } else {
                type_guard.emplace(implicit_type_stack_, *ctx_.pool.strip_volatile(explicit_type));
                resolving_.set_sema_type(id, explicit_type);
                annotation_typed_decl = true;
            }
        }

        // Only update the decl value type if it hasn't been set unless this is an instantiation
        // re-typing a body-local decl (whose annotation, if any, was just re-set above)
        if (decl.value) {
            {
                const mutating_context_guard dyn_g{dyn_unsized_ok_,
                                                   decl.value->is<ast::type_expr>()};
                resolve(*decl.value);
            }
            if (last_type_->is_poison()) { return poison_out(); }
            if (reject_loop_as_value(*decl.value)) { return poison_out(); }
            if (annotation_typed_decl &&
                reject_type_as_value(*decl.value, resolving_.get_sema_type(id))) {
                return poison_out();
            }
            // A global, or a `const` that folds, is never stored, so nothing else checks it
            if (annotation_typed_decl &&
                (return_trackers_.empty() || !decl.has_modifier(ast::decl_modifiers::LET)) &&
                reject_unassignable_global_initializer(
                    *decl.value, resolving_.get_sema_type(id), !return_trackers_.empty())) {
                return poison_out();
            }
            if (decl.value->is<ast::identifier_expr>() &&
                (*decl.value)->get_token_type() == syntax::token_type_t::AUTO_TYPE) {
                last_type_.emplace(ctx_.poison_node(resolving_,
                                                    id,
                                                    "Type aliases cannot be 'auto'",
                                                    error::ILLEGAL_AUTO_USAGE,
                                                    resolving_.ast.location_of(*decl.value)));
                return poison_out();
            }
            gsl::not_null decl_value_type_p{last_type_.take()};
            // `const X = MakesAType()`: fold the constructor now, exactly like an annotation
            if (decl_value_type_p->get_data().is<types::deferred_call>()) {
                const auto& dc_call{decl_value_type_p->get_data().as<types::deferred_call>().call};
                gir::const_eval evaluator{ctx_, resolving_};
                decl_value_type_p =
                    &denoted_type(evaluator.force_deferred_call(*decl_value_type_p));
                if (decl_value_type_p->is_poison()) {
                    last_type_.emplace(*decl_value_type_p);
                    return poison_out();
                }
                register_non_generic_type_ctor_members(*decl_value_type_p, dc_call);
            }
            auto& decl_value_type{*decl_value_type_p};
            auto& normalized_val_type{
                decl.explicit_type ? decl_value_type : *ctx_.pool.strip_volatile(decl_value_type)};
            if (reresolve_local && !annotation_typed_decl) {
                resolving_.set_sema_type(id, normalized_val_type);
            } else {
                resolving_.set_sema_type_if(id, normalized_val_type);
            }
        }

        if (!resolving_.has_sema_type(id)) {
            last_type_.emplace(ctx_.get_poison());
            return poison_out();
        }

        // A `let mut` (or explicit `: auto`) binding whose inferred type is `comptime_*` has no
        // stable place to stay comptime: materialize it to its runtime peer now
        const bool wants_concrete{decl.has_modifier(ast::decl_modifiers::MUT) ||
                                  (decl.explicit_type && decl.explicit_type->get_token_type() ==
                                                             syntax::token_type_t::AUTO_TYPE)};
        if (wants_concrete) {
            auto& dt{resolving_.get_sema_type(id)};
            // Spelled out, an untyped annotation asks for what only exists at compile time
            if (is_comptime_numeric(dt.get_kind()) && annotation_typed_decl &&
                !decl.has_modifier(ast::decl_modifiers::COMPTIME)) {
                ctx_.poison_symbol(sym,
                                   fmt::format("'{}' only exists at compile time, so it cannot be "
                                               "stored in a 'let mut' binding; use 'comptime let "
                                               "mut'",
                                               ctx_.type_display_name(dt)),
                                   error::COMPILE_TIME_ONLY_VALUE,
                                   resolving_.ast.location_of(id));
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
            if (is_comptime_numeric(dt.get_kind())) {
                resolving_.set_sema_type(id, comptime_numeric_view(dt));
            }
        }
    }

    // The symbol's kind can be fully resolved with knowledge of the declarations type
    auto&       resolved_type{resolving_.get_sema_type(id)};
    const auto& type_data{resolved_type.get_data()};

    if (resolved_type.is_poison()) {
        sym.set_kind(symbol_kind::POISONED);
    } else {
        // No field-level path covers `extern` fn signatures / globals themselves. Check here.
        if (decl.has_modifier(ast::decl_modifiers::EXTERN)) {
            const auto bad{scan_cabi_offenders(resolved_type, target_has_x86_fp80())};
            if (bad.has_nonabi_scalar) {
                const auto loc{decl.explicit_type ? resolving_.ast.location_of(*decl.explicit_type)
                                                  : resolving_.ast.location_of(id)};
                ctx_.poison_symbol(sym,
                                   cabi_nonabi_scalar(ctx_.type_display_name(resolved_type), loc));
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
        }
        if (decl.has_modifier(ast::decl_modifiers::EXTERN) ||
            decl.has_modifier(ast::decl_modifiers::EXPORT)) {
            if (const auto vec{find_nonabi_vector(resolved_type, target_ptr_bits())}) {
                const auto loc{decl.explicit_type ? resolving_.ast.location_of(*decl.explicit_type)
                                                  : resolving_.ast.location_of(id)};
                ctx_.poison_symbol(sym, nonabi_vector(ctx_, *vec, loc));
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
        }
        if (decl.has_modifier(ast::decl_modifiers::EXTERN) ||
            decl.has_modifier(ast::decl_modifiers::EXPORT)) {
            const auto loc{decl.explicit_type ? resolving_.ast.location_of(*decl.explicit_type)
                                              : resolving_.ast.location_of(id)};
            if (auto problem{unmodeled_c_abi_error(resolved_type, loc)}) {
                ctx_.poison_symbol(sym, std::move(*problem));
                resolving_.set_sema_type(decl.name, ctx_.get_poison());
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
        }

        resolve_decl_attributes(id, decl, type_data);
        if (const auto runtime{const_closure_runtime_capture(decl)}) {
            // A pack element is captured as `rest#k`, which the source spells `rest[k]`
            const auto element{pack_element::parse(*runtime)};
            const auto spelled{element ? element->spelled() : std::string{*runtime}};
            ctx_.poison_symbol(sym,
                               fmt::format("A 'const' function cannot capture the runtime value "
                                           "'{}'; declare the closure with 'let'",
                                           spelled),
                               error::ILLEGAL_BINDING_KIND,
                               resolving_.ast.location_of(id));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        const bool literal_type_anno{decl.explicit_type && decl.explicit_type->get_token_type() ==
                                                               syntax::token_type_t::TYPE_TYPE};
        const bool binds_type{decl.value && decl_value_denotes_type(*decl.value)};
        if (literal_type_anno && decl.value && !binds_type &&
            operand_nature(*decl.value) == operand_nature_t::VALUE) {
            ctx_.poison_symbol(
                sym,
                fmt::format("A 'type' binding needs a type, but was given a value of type '{}'",
                            ctx_.type_display_name(resolving_.get_sema_type(*decl.value))),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(*decl.value));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }
        // Only a compile-time binding (`const` or `comptime let mut`) holds what exists solely at
        // compile time
        const bool runtime_binding{!decl.has_modifier(ast::decl_modifiers::COMPTIME)};
        if (runtime_binding && (literal_type_anno || binds_type)) {
            ctx_.poison_symbol(sym,
                               "a 'type' value cannot be stored in a 'let' or 'let mut' binding; "
                               "use 'const' instead",
                               error::MUTABLE_TYPE_BINDING,
                               resolving_.ast.location_of(id));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        // `const U = undefined` aliases the literal. There is no runtime value to store
        const bool binds_undefined{resolved_type.get_kind() == type_kind::UNDEFINED};
        if (binds_undefined && (runtime_binding || decl.has_modifier(ast::decl_modifiers::MUT))) {
            ctx_.poison_symbol(sym,
                               "'undefined' only exists at compile time, so it cannot be stored in "
                               "a 'let' or 'let mut' binding; use 'const' to alias it",
                               error::COMPILE_TIME_ONLY_VALUE,
                               resolving_.ast.location_of(id));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        const bool aggregate_literal{
            decl.value &&
            decl.value
                ->any<ast::struct_expr, ast::union_expr, ast::enum_expr, ast::interface_expr>()};
        const bool comptime_value{!binds_type && !aggregate_literal &&
                                  is_comptime_aggregate(resolved_type)};
        if (comptime_value && runtime_binding) {
            ctx_.poison_symbol(sym,
                               resolved_type.get_kind() == type_kind::ENUM_LITERAL
                                   ? std::string{"an enum literal only exists at compile time, so "
                                                 "it cannot be stored in a 'let' or 'let mut' "
                                                 "binding; give the binding an enum type "
                                                 "(`let x: E = .name;`) or use 'const' instead"}
                                   : fmt::format("a value of type '{}' holds 'type's and only "
                                                 "exists at compile time, so it cannot be stored "
                                                 "in a 'let' or 'let mut' binding; use 'const' "
                                                 "instead",
                                                 ctx_.type_display_name(resolved_type)),
                               error::MUTABLE_TYPE_BINDING,
                               resolving_.ast.location_of(id));
            resolving_.set_sema_type(decl.name, ctx_.get_poison());
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        // `reresolve_local` re-derives this decl's kind fresh even if a stale pass over this same
        // generic instantiation already set it
        if (binds_type && !type_data.is<types::module_t>()) {
            // A type alias is a type even when its value is spelled like a callable (`fn(...): T`)
            sym.set_kind(symbol_kind::TYPE);
        } else if (!sym.has_kind() || reresolve_local) {
            if (type_data.is<types::builtin_function>() || type_data.is<types::function>()) {
                sym.set_kind(symbol_kind::CALLABLE);
            } else if (resolved_type == ctx_.get_builtin_resolved_type(type_kind::TYPE)) {
                sym.set_kind(symbol_kind::TYPE);
            } else if (type_data.is<types::module_t>()) {
                sym.set_kind(symbol_kind::MODULE);
            } else {
                sym.set_kind(symbol_kind::VALUE);
            }
        }

        // Aliases of an existing or constructed type have no storage. Aggregate literals keep
        // their own member emission path
        using storageless_kind = mod::storageless_kind;
        const auto storageless{[&] -> stdx::option<storageless_kind> {
            if ((binds_type && !aggregate_literal) || type_data.is<types::module_t>()) {
                return storageless_kind::ALIAS;
            }
            if ((comptime_value || binds_undefined) &&
                !decl.has_modifier(ast::decl_modifiers::MUT)) {
                return storageless_kind::COMPTIME_VALUE;
            }
            return stdx::none;
        }()};
        resolving_.storageless_decls.insert_or_assign(id.get_index(), storageless);

        // Only the function written here takes this name. Another one bound to it keeps its own
        if (type_data.is<types::function>() && decl.value && decl.value->is<ast::function_expr>()) {
            const auto& decl_ident{resolving_.ast.get_as<ast::identifier_expr>(decl.name)};
            ctx_.generic_functions.set_function_name(resolved_type, decl_ident.name);
        }

        // Remember the declared name of a user aggregate so `@typeName` can report it
        if (decl.value &&
            decl.value
                ->any<ast::struct_expr, ast::union_expr, ast::enum_expr, ast::interface_expr>()) {
            if (const auto value_type{resolving_.get_sema_type_opt(*decl.value)};
                value_type && !value_type->is_poison()) {
                const auto& decl_ident{resolving_.ast.get_as<ast::identifier_expr>(decl.name)};
                ctx_.user_type_names.try_emplace(value_type.get(), decl_ident.name);
            }
        }
    }

    if (reresolve_local) {
        resolving_.set_sema_type(decl.name, resolved_type);
    } else {
        resolving_.set_sema_type_if(decl.name, resolved_type);
    }
    resolving_.set_identifier_declaration(
        decl.name, declaration_ref{.owner = &resolving_, .decl = id, .annotation = stdx::none});
    sym.set_status(symbol_status::RESOLVED);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::discard_stmt& discard) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(discard.discarded);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::expr_stmt& expr) -> void {
    PROFILE_FUNCTION();
    // An `if`/`match` branch statement yields the branch's value, which is only discarded when
    // the `if`/`match` itself is
    const bool is_unused{!in_expr_branch_ || arm_of_unused_};
    if (is_unused) { unused_value_nodes_.insert(ast::node_id{expr.expression}.get_index()); }
    const mutating_context_guard operand_g{arm_of_unused_, false};
    TRY_RESOLVE(expr.expression);
    resolving_.set_sema_type(expr.expression, *last_type_.take());
    if (is_unused) { check_unused_result(id, expr); }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::import_stmt& import_stmt) -> void {
    PROFILE_FUNCTION();
    const auto named{import_stmt.get_name(resolving_.ast)};

    // An unaliased test-block import exists only to pull the target module's tests into discovery
    if (!named) {
        auto& import_type{resolving_.get_sema_type(id)};
        if (const auto mod_opt{import_type.get_data().as_opt<types::module_t>()}) {
            context new_ctx{ctx_};
            resolve_types(mod_opt->imported, new_ctx);
        }
        return last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
    }

    const auto [ident_id, name]{*named};
    // The collector rejected the binding (e.g. it shadows another import) and said why
    const auto sym_opt{ctx_.registry.get_from_opt(table_idx_, name)};
    if (!sym_opt) {
        last_type_.emplace(ctx_.get_poison());
        return resolving_.set_sema_type(ident_id, *last_type_);
    }
    auto& sym{*sym_opt};

    const auto poison_out = [&] -> void {
        last_type_.emplace(ctx_.get_poison());
        resolving_.set_sema_type(ident_id, *last_type_);
        ctx_.poison_symbol(sym);
    };

    // Updating this type reflects on the symbol in the actual table as well
    auto& import_type{resolving_.get_sema_type(id)};
    if (import_type.is_poison()) { return poison_out(); }
    ASSERT(import_type.is_resolved(), "Import types should be resolved on pass 1");
    const auto mod_opt{import_type.get_data().as_opt<types::module_t>()};
    if (!mod_opt) { return poison_out(); }
    auto& module{*mod_opt};

    // There's no need to poison the import type since it would lose all of the module information
    context new_ctx{ctx_};
    resolve_types(module.imported, new_ctx);
    if (module.imported.is_poisoned()) {
        last_type_.emplace(ctx_.get_poison());
        resolving_.set_sema_type(ident_id, *last_type_);
        return ctx_.poison_symbol(
            sym,
            fmt::format("Import '{}' failed to resolve due to errors it contains", name),
            error::IMPORTED_MODULE_CONTAINS_ERRORS,
            resolving_.ast.location_of(id));
    }

    // Give the alias identifier the module type so the LSP can hover over it
    resolving_.set_sema_type_if(ident_id, import_type);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::test_stmt& test) -> void {
    PROFILE_FUNCTION();
    // Only a test build runs tests, so only it checks them (they may use `@cfg(testing)` code)
    if (!ctx_.testing) {
        return last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
    }
    auto& test_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!test_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    const scope s{table_stack_, test_type.get_symbol_table_idx(), table_idx_};

    // Duplicate test blocks are a big no-no if named
    if (test.description) {
        const auto& description{resolving_.ast.get_as<ast::string_expr>(*test.description)};
        const auto& name{description.value};
        auto [it, inserted]{named_tests_.try_emplace(name, id)};
        if (!inserted) {
            const auto original_loc{resolving_.ast.location_of(it->second)};
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Duplicate test block named '{}'; previous declaration here: {}",
                            name,
                            original_loc),
                error::DUPLICATE_TEST_NAME,
                resolving_.ast.location_of(id)));
        }
    }

    // A test body is a function of its own, so a closure in it captures the test's locals
    const function_boundary_guard fn_boundary{function_boundaries_, table_stack_.size() - 1};
    const open_function_guard     fn_node{open_function_nodes_, id};
    const self_recursion_guard    fn_self_ref{self_recursive_flags_, false};

    const auto& block{resolving_.ast.get_as<ast::block_stmt>(test.block)};
    if (resolve_block_statements(block)) { return fail_scoped_body(); }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

} // namespace ghoti::sema
