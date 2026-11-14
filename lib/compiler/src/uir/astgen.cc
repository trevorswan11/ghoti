#include "compiler/uir/astgen.hh"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/fixed/vector.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "astgen_internal.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/token_type.hh"
#include "compiler/uir/uir.hh"
#include "support/diagnostic.hh"
#include "support/string_utils.hh"

namespace ghoti::uir {

namespace detail {

namespace {

// Modifiers that only mean something on a module-scope or aggregate member declaration
auto function_local_illegal_modifiers(const ast::decl_stmt& decl) {
    using ast::decl_modifiers;
    static constexpr std::array<std::pair<decl_modifiers, std::string_view>, 5> disallowed{{
        {decl_modifiers::PUBLIC, syntax::keywords::PUBLIC.name},
        {decl_modifiers::EXTERN, syntax::keywords::EXTERN.name},
        {decl_modifiers::EXPORT, syntax::keywords::EXPORT.name},
        {decl_modifiers::THREADLOCAL, syntax::keywords::THREADLOCAL.name},
        {decl_modifiers::WEAK, syntax::keywords::WEAK.name},
    }};

    stdx::fixed::vector<std::string_view, disallowed.size()> found;
    for (const auto& [flag, spelling] : disallowed) {
        if (decl.has_modifier(flag)) { found.emplace_back(spelling); }
    }
    return found;
}

// The first linkage modifier on `decl`, which needs a symbol to attach to
auto linkage_modifier(const ast::decl_stmt& decl) -> stdx::option<std::string_view> {
    using ast::decl_modifiers;
    static constexpr std::array<std::pair<decl_modifiers, std::string_view>, 4> linkage{{
        {decl_modifiers::EXTERN, syntax::keywords::EXTERN.name},
        {decl_modifiers::EXPORT, syntax::keywords::EXPORT.name},
        {decl_modifiers::WEAK, syntax::keywords::WEAK.name},
        {decl_modifiers::THREADLOCAL, syntax::keywords::THREADLOCAL.name},
    }};
    for (const auto& [flag, spelling] : linkage) {
        if (decl.has_modifier(flag)) { return spelling; }
    }
    return stdx::none;
}

// A function literal, or an `extern` function signature
auto declares_function(const ast::decl_stmt& decl) -> bool {
    if (decl.value) { return decl.value->is<ast::function_expr>(); }
    return decl.explicit_type &&
           (decl.explicit_type->get_token_type() == syntax::token_type_t::FUNCTION ||
            decl.explicit_type->get_token_type() == syntax::token_type_t::EXTERN);
}

auto is_aggregate_value(ast::expr_handle value) -> bool {
    return value.any<ast::struct_expr, ast::union_expr, ast::enum_expr, ast::interface_expr>();
}

} // namespace

astgen::astgen(const mod::module_t& mod, const astgen_context& cx)
    : mod_{mod}, ast_{mod.ast}, cx_{cx}, cfg_{mod, cx.cfg, diags_} {}

auto astgen::run() -> lowered {
    PROFILE_FUNCTION();
    collect_mutation_sites();
    push_scope(scope_kind::MODULE);
    cfg_.gather_cfg_values(ast_.get_roots());
    cfg_.resolve_all_cfg_values();
    lower_roots();
    pop_scope();
    // A defer copied onto several exits reports its errors once
    diags_.remove_duplicates();
    return {std::move(out_), std::move(diags_)};
}

auto astgen::lower_roots() -> void {
    std::vector<ast::stmt_handle> roots;
    roots.reserve(ast_.get_roots().size());
    for (const auto id : ast_.get_roots()) { roots.emplace_back(id); }

    std::vector<pending_decl> pending;
    declare_module_items(roots, pending);
    for (const auto& item : pending) { lower_pending(item); }
}

auto astgen::declare_module_items(const std::vector<ast::stmt_handle>& items,
                                  std::vector<pending_decl>&           pending) -> void {
    for (const auto item : spliced(items)) { declare_module_item(item, pending); }
}

auto astgen::add_root_decl(decl d, ast::node_id node, std::vector<pending_decl>& pending) -> u32 {
    const auto index{out_.add_decl(d)};
    out_.root_namespace_mut().decls.emplace_back(index);
    pending.emplace_back<pending_decl>({index, node});
    return index;
}

auto astgen::declare_module_item(ast::stmt_handle item, std::vector<pending_decl>& pending)
    -> void {
    const ast::node_id id{item};
    switch (id.get_kind()) {
    case ast::node_kind::DECL_STATEMENT: {
        const auto& decl{ast_.get_as<ast::decl_stmt>(id)};
        check_decl_modifiers(id, decl, false);
        const auto index{add_value_decl(decl, id, NO_INDEX)};
        out_.root_namespace_mut().decls.emplace_back(index);
        pending.emplace_back<pending_decl>({index, id});
        declare_member(name_of(decl.name),
                       location_of(decl.name),
                       {
                           .kind         = binding_kind::DECL,
                           .decl         = index,
                           .mutable_decl = decl.has_modifier(ast::decl_modifiers::MUT),
                       });
        break;
    }
    case ast::node_kind::IMPORT_STATEMENT: {
        const auto& import{ast_.get_as<ast::import_stmt>(id)};
        const auto  index{add_import_decl(id, import, NO_INDEX)};
        out_.root_namespace_mut().decls.emplace_back(index);
        if (const auto named{import.get_name(ast_)}) {
            declare_member(named->second,
                           import_location(id, import),
                           {.kind = binding_kind::DECL, .decl = index});
        }
        break;
    }
    case ast::node_kind::TEST_STATEMENT: {
        if (!cx_.cfg.testing) { break; }
        const auto& test{ast_.get_as<ast::test_stmt>(id)};
        decl_info   d{.kind = decl_kind::TEST, .node = id};
        if (test.description) {
            d.name = out_.intern(ast_.get_as<ast::string_expr>(*test.description).value);
        }
        DISCARD(add_root_decl(d, id, pending));
        break;
    }
    case ast::node_kind::IMPL_STATEMENT:
        DISCARD(add_root_decl({.kind = decl_kind::IMPL, .node = id}, id, pending));
        break;
    case ast::node_kind::BLOCK_STATEMENT:
        if (!ast_.get_as<ast::block_stmt>(id).is_comptime) {
            report(location_of(id),
                   sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
                   "Cannot have block at the top level");
            break;
        }
        DISCARD(add_root_decl({.kind = decl_kind::COMPTIME, .node = id}, id, pending));
        break;
    case ast::node_kind::EXPRESSION_STATEMENT:
    case ast::node_kind::DISCARD_STATEMENT:
        if (check_top_level_statement(id)) {
            DISCARD(add_root_decl({.kind = decl_kind::COMPTIME, .node = id}, id, pending));
        }
        break;
    case ast::node_kind::DEFER_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
               "Cannot have defer outside of a function's scope");
        break;
    case ast::node_kind::ERRDEFER_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
               "Cannot have errdefer outside of a function's scope");
        break;
    case ast::node_kind::RETURN_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_CONTROL_FLOW,
               "Cannot return outside of a function");
        break;
    case ast::node_kind::BREAK_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_CONTROL_FLOW,
               "Cannot break outside of a loop or label");
        break;
    case ast::node_kind::CONTINUE_STATEMENT:
        report(location_of(id),
               sema::error::ILLEGAL_CONTROL_FLOW,
               "Cannot continue outside of a loop");
        break;
    default: UNREACHABLE("a `@cfg` statement is spliced before its items are declared");
    }
}

auto astgen::lower_pending(const pending_decl& item) -> void {
    const auto kind{out_.decl_at(item.decl).kind};
    switch (kind) {
    case decl_kind::VALUE:
        lower_decl_bodies(item.decl, ast_.get_as<ast::decl_stmt>(item.node));
        break;
    case decl_kind::TEST:     lower_test(item.decl, ast_.get_as<ast::test_stmt>(item.node)); break;
    case decl_kind::IMPL:     lower_impl(item.decl, ast_.get_as<ast::impl_stmt>(item.node)); break;
    case decl_kind::COMPTIME: {
        functions_.emplace_back();
        std::vector<u32> instructions;
        {
            const body_guard guard{*this, instructions};
            if (const auto block{ast_.get_as_opt<ast::block_stmt>(item.node)}) {
                auto body{scoped_block(ast::block_handle{item.node})};
                yields(
                    add_pl(tag::block_comptime, writer().str_field({}).body_field(body), item.node),
                    item.node);
            } else {
                DISCARD(stmt(ast::stmt_handle{item.node}));
                yields(to_ref(well_known::void_value), item.node);
            }
        }
        functions_.pop_back();
        out_.decl_at_mut(item.decl).value_body = out_.add_body(instructions);
        break;
    }
    case decl_kind::IMPORT: break;
    }
}

auto astgen::lower_decl_bodies(u32 index, const ast::decl_stmt& decl) -> void {
    functions_.emplace_back();
    const auto pop_state{gsl::finally([this] { functions_.pop_back(); })};
    const bool initializes_fn{decl.value && decl.value->is<ast::function_expr>()};

    std::vector<u32> type_instructions;
    if (decl.explicit_type) { type_instructions = type_body(*decl.explicit_type); }

    std::vector<u32> attr_instructions;
    lower_attributes(decl.attributes,
                     initializes_fn ? stdx::option<bool>{false} : stdx::none,
                     attr_instructions);

    std::vector<u32> value_instructions;
    if (decl.value) {
        const body_guard guard{*this, value_instructions};
        const auto       value{*decl.value};
        ref              result{};
        if (initializes_fn) {
            result =
                lower_function(ast::node_id{value}, ast_.get_as<ast::function_expr>(value), decl);
        } else {
            if (is_aggregate_value(value)) { pending_type_name_ = name_of(decl.name); }
            result             = expr(value);
            pending_type_name_ = {};
        }
        yields(result, ast::node_id{value});
    }

    auto& d{out_.decl_at_mut(index)};
    d.type_body  = out_.add_body(type_instructions);
    d.attrs_body = out_.add_body(attr_instructions);
    d.value_body = out_.add_body(value_instructions);
}

auto astgen::decl_flags_of(const ast::decl_stmt& decl) const -> decl_flags {
    using ast::decl_modifiers;
    decl_flags flags{decl_flags::NONE};
    const auto set{[&](decl_modifiers modifier, decl_flags flag) {
        if (decl.has_modifier(modifier)) { flags |= flag; }
    }};
    set(decl_modifiers::PUBLIC, decl_flags::PUBLIC);
    set(decl_modifiers::EXTERN, decl_flags::EXTERN);
    set(decl_modifiers::EXPORT, decl_flags::EXPORT);
    set(decl_modifiers::THREADLOCAL, decl_flags::THREADLOCAL);
    set(decl_modifiers::WEAK, decl_flags::WEAK);
    set(decl_modifiers::LET, decl_flags::LET);
    set(decl_modifiers::MUT, decl_flags::MUT);
    set(decl_modifiers::COMPTIME, decl_flags::COMPTIME);
    if (decl.has_modifier(decl_modifiers::COMPTIME) && !decl.has_modifier(decl_modifiers::MUT)) {
        flags |= decl_flags::CONSTANT;
    }
    if (decl.is_synthesized) { flags |= decl_flags::SYNTHESIZED; }
    return flags;
}

auto astgen::add_value_decl(const ast::decl_stmt& decl, ast::node_id node, u32 owner) -> u32 {
    decl_info d{
        .kind  = decl_kind::VALUE,
        .name  = out_.intern(name_of(decl.name)),
        .node  = node,
        .flags = decl_flags_of(decl),
        .owner = owner,
    };
    if (decl.extern_target) {
        d.extern_target = out_.intern(ast_.get_as<ast::string_expr>(*decl.extern_target).value);
    }
    if (decl.link_name) {
        d.link_name = out_.intern(ast_.get_as<ast::string_expr>(*decl.link_name).value);
    }
    return out_.add_decl(d);
}

auto astgen::add_import_decl(ast::node_id node, const ast::import_stmt& import, u32 owner) -> u32 {
    const auto named{import.get_name(ast_)};
    decl_info  d{
        .kind         = decl_kind::IMPORT,
        .name         = out_.intern(named ? named->second : std::string_view{}),
        .node         = node,
        .flags        = ast::import_stmt::is_public(node) ? decl_flags::PUBLIC : decl_flags::NONE,
        .import_index = add_import(node, import),
        .owner        = owner,
    };
    return out_.add_decl(d);
}

auto astgen::add_import(ast::node_id node, const ast::import_stmt& import) -> u32 {
    import_info info{.node = node};
    const auto  payload{ast::node_id{import.payload}};
    auto        loaded{[&] {
        if (const auto path{ast_.get_as_opt<ast::string_expr>(payload)}) {
            info.spec = std::string{path->value};
            return cx_.modules.try_get_file_module(path->value, mod_.parent_path);
        }
        info.spec       = std::string{ast_.get_as<ast::identifier_expr>(payload).name};
        info.is_library = true;
        return cx_.modules.try_get_library_module(info.spec);
    }()};
    if (loaded) {
        info.path.emplace((*loaded)->path);
    } else {
        diags_.emplace_back(
            loaded.error().get_message(), sema::error::MODULE_LOAD_ERROR, location_of(payload));
    }
    return out_.add_import(std::move(info));
}

auto astgen::lower_test(u32 index, const ast::test_stmt& test) -> void {
    functions_.emplace_back<function_state>({.allows_defer = true, .testing = true});
    const auto       pop_state{gsl::finally([this] { functions_.pop_back(); })};
    std::vector<u32> instructions;
    {
        const body_guard guard{*this, instructions};
        auto             body{scoped_block(test.block, scope_kind::TEST)};
        yields(
            add_pl(tag::block, writer().str_field({}).body_field(body), ast::node_id{test.block}),
            ast::node_id{test.block});
    }
    out_.decl_at_mut(index).value_body = out_.add_body(instructions);
}

auto astgen::lower_impl(u32 index, const ast::impl_stmt& impl) -> void {
    functions_.emplace_back();
    const auto pop_state{gsl::finally([this] { functions_.pop_back(); })};
    const auto node{*out_.decl_at(index).node};

    std::vector<u32> instructions;
    const body_guard guard{*this, instructions};
    const auto       inst{reserve(node)};
    {
        const scope_guard scoped{*this, scope_kind::NAMESPACE};
        scoped.get().is_aggregate = true;

        std::vector<u32> params;
        {
            const body_guard params_guard{*this, params};
            for (const auto& param : impl.impl_params) { lower_param(param, 0); }
        }
        std::vector<u32> iface;
        if (impl.interface_type) { iface = type_body(*impl.interface_type); }
        const auto target{type_body(impl.target_type)};
        const auto decls{namespace_members(inst, impl.members)};

        auto record{writer()};
        record.body_field(params).body_field(iface).body_field(target).count(decls.size());
        for (const auto d : decls) { record.u32_field(d); }
        fill_pl(inst, tag::impl_decl, record);
    }
    yields(inst_ref(inst), node);
    out_.decl_at_mut(index).value_body = out_.add_body(instructions);
}

auto astgen::check_top_level_statement(ast::node_id id) -> bool {
    const auto expr_stmt{ast_.get_as_opt<ast::expr_stmt>(id)};
    if (!expr_stmt) { return true; }
    using ast::node_kind;
    const auto report_control_flow{[&] {
        report(location_of(id),
               sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
               "Control-flow constructs are not allowed as statements at the top level; "
               "use `@cfg` for conditional declarations, or bind the value with `const x = ...`");
        return false;
    }};
    switch (expr_stmt->expression->get_kind()) {
    case node_kind::IF_EXPRESSION:
    case node_kind::MATCH_EXPRESSION:
    case node_kind::WHILE_LOOP_EXPRESSION:
    case node_kind::DO_WHILE_LOOP_EXPRESSION:
    case node_kind::FOR_LOOP_EXPRESSION:
    case node_kind::INFINITE_LOOP_EXPRESSION: return report_control_flow();
    case node_kind::LABEL_EXPRESSION:
        if (!ast_.get_as<ast::label_expr>(expr_stmt->expression).is_comptime(ast_)) {
            return report_control_flow();
        }
        return true;
    default: return true;
    }
}

auto astgen::check_decl_modifiers(ast::node_id id, const ast::decl_stmt& decl, bool function_local)
    -> void {
    if (function_local) {
        if (const auto illegal{function_local_illegal_modifiers(decl)}; !illegal.empty()) {
            report(location_of(id),
                   sema::error::ILLEGAL_LOCAL_DECL_MODIFIER,
                   "Modifier{} '{}' cannot be used on a declaration local to a function",
                   string_utils::plural_suffix(illegal.size()),
                   fmt::join(illegal, "', '"));
        }
    }

    // Linkage attaches to a function itself, which `const` binds; data needs `let`'s storage
    if (const auto linkage{linkage_modifier(decl)}) {
        const bool is_fn{declares_function(decl)};
        const bool is_comptime{decl.has_modifier(ast::decl_modifiers::COMPTIME)};
        if (is_fn && (!is_comptime || decl.has_modifier(ast::decl_modifiers::MUT))) {
            report(location_of(id),
                   sema::error::ILLEGAL_BINDING_KIND,
                   "A '{}' function is declared with 'const'",
                   *linkage);
        } else if (!is_fn && is_comptime) {
            report(location_of(id),
                   sema::error::ILLEGAL_BINDING_KIND,
                   "'{}' needs storage, which a compile-time declaration lacks; use 'let' or "
                   "'let mut'",
                   *linkage);
        } else if (is_fn && *linkage == syntax::keywords::THREADLOCAL.name) {
            report(location_of(id),
                   sema::error::ILLEGAL_BINDING_KIND,
                   "A function cannot be 'threadlocal'");
        }
    }

    if (!decl.value) { return; }
    const auto value{*decl.value};
    if (is_aggregate_value(value)) {
        if (!decl.has_modifier(ast::decl_modifiers::COMPTIME) ||
            decl.has_modifier(ast::decl_modifiers::MUT)) {
            report(location_of(id),
                   sema::error::ILLEGAL_NON_CONST_STATEMENT,
                   "All {}s must be declared with 'const'",
                   value->display_name());
        }
    } else if (value.is<ast::function_expr>() && decl.has_modifier(ast::decl_modifiers::MUT)) {
        report(location_of(id),
               sema::error::ILLEGAL_NON_CONST_STATEMENT,
               "A function declaration is either 'const' or 'let'");
    }
}

auto astgen::selected_items(ast::node_id id, const ast::cfg_stmt& cfg)
    -> const std::vector<ast::stmt_handle>& {
    static const std::vector<ast::stmt_handle> NONE;
    auto [it, inserted]{cfg_arms_.try_emplace(id.get_index())};
    if (inserted) {
        if (const auto arm{cfg_.select_arm_of(cfg.arms)}) {
            it->second.emplace(static_cast<usize>(arm.get() - cfg.arms.data()));
            cfg_.fire_eager_compile_errors(arm->items);
        }
    }
    if (!it->second) { return NONE; }
    return cfg.arms[*it->second].items;
}

auto astgen::spliced(const std::vector<ast::stmt_handle>& items) -> std::vector<ast::stmt_handle> {
    std::vector<ast::stmt_handle> out;
    out.reserve(items.size());
    for (const auto item : items) {
        const ast::node_id id{item};
        if (const auto cfg{ast_.get_as_opt<ast::cfg_stmt>(id)}) {
            for (const auto inner : spliced(selected_items(id, *cfg))) { out.emplace_back(inner); }
            continue;
        }
        out.emplace_back(item);
    }
    return out;
}

template <typename Group, typename Item>
auto astgen::spliced_group(const std::vector<Item>& items, const std::vector<Group>& groups)
    -> std::vector<Item> {
    std::vector<Item> out{items};
    usize             offset{0};
    for (const auto& group : groups) {
        const auto arm{cfg_.select_arm_of(group.arms)};
        if (!arm) { continue; }
        const auto at{static_cast<idiff>(group.position + offset)};
        out.insert(out.begin() + at, arm->items.begin(), arm->items.end());
        offset += arm->items.size();
    }
    return out;
}

template auto astgen::spliced_group(const std::vector<ast::struct_expr::field>&,
                                    const std::vector<ast::struct_expr::cfg_group>&)
    -> std::vector<ast::struct_expr::field>;
template auto astgen::spliced_group(const std::vector<ast::union_expr::field>&,
                                    const std::vector<ast::union_expr::cfg_group>&)
    -> std::vector<ast::union_expr::field>;
template auto astgen::spliced_group(const std::vector<ast::enum_expr::enumeration>&,
                                    const std::vector<ast::enum_expr::cfg_group>&)
    -> std::vector<ast::enum_expr::enumeration>;
template auto astgen::spliced_group(const ast::member_list&,
                                    const std::vector<ast::struct_expr::member_cfg_group>&)
    -> ast::member_list;

auto astgen::lower_attributes(const stdx::option<ast::attribute_list>& attributes,
                              stdx::option<bool>                       routed,
                              std::vector<u32>&                        into) -> void {
    if (!attributes) { return; }
    const body_guard guard{*this, into};
    for (const auto& item : attributes->items) {
        if (routed && ast::routes_to_fn_literal(item.kind) != *routed) { continue; }
        std::vector<ref> args;
        args.reserve(item.args.size());
        for (const auto arg : item.args) { args.emplace_back(expr(arg)); }
        DISCARD(add_pl(tag::attribute,
                       writer().u32_field(static_cast<u32>(item.kind)).refs_field(args),
                       ast::node_id{item.name}));
    }
}

auto astgen::import_location(ast::node_id id, const ast::import_stmt& import) const
    -> source_location {
    if (import.alias) { return location_of(*import.alias); }
    return location_of(id);
}

auto astgen::name_of(ast::identifier_handle h) const -> std::string_view {
    return ast_.get_as<ast::identifier_expr>(h).name;
}

auto astgen::collect_mutation_sites() -> void {
    const auto root_name{[this](ast::node_id at) -> stdx::option<std::string_view> {
        while (true) {
            if (const auto ident{ast_.get_as_opt<ast::identifier_expr>(at)}) { return ident->name; }
            if (const auto dot{ast_.get_as_opt<ast::dot_expr>(at)}) {
                at = ast::node_id{dot->object};
                continue;
            }
            if (const auto index{ast_.get_as_opt<ast::index_expr>(at)}) {
                at = ast::node_id{index->array};
                continue;
            }
            return stdx::none;
        }
    }};
    const auto is_mut_token{[](syntax::token_type_t t) {
        using syntax::token_type_t;
        return t == token_type_t::AND_MUT || t == token_type_t::CARET_MUT ||
               t == token_type_t::AND_MUT_POLY || t == token_type_t::CARET_MUT_POLY;
    }};
    for (const auto id : ast_.all_nodes()) {
        stdx::option<ast::node_id> written;
        if (const auto assign{ast_.get_as_opt<ast::assignment_expr>(id)}) {
            written.emplace(assign->lhs);
        } else if (const auto ref{ast_.get_as_opt<ast::reference_expr>(id)};
                   ref && is_mut_token(id.get_token_type())) {
            written.emplace(ref->rhs);
        } else if (const auto addr{ast_.get_as_opt<ast::address_of_expr>(id)};
                   addr && is_mut_token(id.get_token_type())) {
            written.emplace(addr->rhs);
        }
        if (!written) { continue; }
        if (const auto name{root_name(*written)}) {
            mutation_sites_.emplace_back(location_of(id), *name);
        }
    }
    std::ranges::sort(mutation_sites_, {}, &std::pair<source_location, std::string_view>::first);
}

auto astgen::mutated_within(ast::node_id fn) const
    -> ankerl::unordered_dense::set<std::string_view> {
    ankerl::unordered_dense::set<std::string_view> names;
    const auto                                     start{location_of(fn)};
    const auto                                     end{ast_.end_location_of(fn)};
    auto                                           it{std::ranges::lower_bound(
        mutation_sites_, start, {}, &std::pair<source_location, std::string_view>::first)};
    for (; it != mutation_sites_.end() && it->first < end; ++it) { names.insert(it->second); }
    return names;
}

auto astgen::add(tag t, data d, source src) -> ref {
    const auto index{out_.add(t, d, src)};
    body_->emplace_back(index);
    return inst_ref(index);
}

auto astgen::add_un(tag t, ref operand, u32 aux, source src) -> ref {
    data d{};
    d.un = {operand, aux};
    return add(t, d, src);
}

auto astgen::add_bin(tag t, ref lhs, ref rhs, source src) -> ref {
    data d{};
    d.bin = {lhs, rhs};
    return add(t, d, src);
}

auto astgen::add_str(tag t, std::string_view text, source src) -> ref {
    data d{};
    d.str = out_.intern(text);
    return add(t, d, src);
}

auto astgen::add_pl(tag t, record_writer& record, source src) -> ref {
    data d{};
    d.pl = {record.finish(), 0};
    ASSERT(schema_of(t).empty() || record_end(out_, t, d.pl.extra) == out_.extra().size(),
           "a UIR record doesn't match its tag's schema");
    return add(t, d, src);
}

auto astgen::reserve(source src) -> u32 { return out_.reserve(src); }

auto astgen::fill_pl(u32 inst, tag t, record_writer& record) -> void {
    data d{};
    d.pl = {record.finish(), 0};
    ASSERT(record_end(out_, t, d.pl.extra) == out_.extra().size(),
           "a UIR record doesn't match its tag's schema");
    out_.set(inst, t, d);
    body_->emplace_back(inst);
}

auto astgen::yields(ref value, source src) -> void { DISCARD(add_un(tag::yield, value, 0, src)); }

} // namespace detail

auto lower_module(const mod::module_t& mod, const astgen_context& cx) -> lowered {
    detail::astgen gen{mod, cx};
    return gen.run();
}

auto lower_reachable(const mod::module_t& root, const astgen_context& cx)
    -> std::vector<lowered_module> {
    std::vector<lowered_module>                                       out;
    ankerl::unordered_dense::set<gsl::not_null<const mod::module_t*>> seen{&root};
    std::vector<gsl::not_null<const mod::module_t*>>                  pending{&root};
    while (!pending.empty()) {
        const auto next{pending.back()};
        pending.pop_back();
        if (next->is_errored()) { continue; }
        auto lowered{lower_module(*next, cx)};
        for (const auto& import : lowered.uir.imports()) {
            if (!import.path) { continue; }
            const auto loaded{cx.modules.find_file_module(*import.path)};
            if (loaded && seen.insert(loaded.get()).second) { pending.emplace_back(loaded.get()); }
        }
        out.emplace_back<lowered_module>({next, std::move(lowered)});
    }
    return out;
}

auto lower(const mod::module_t& mod, const astgen_context& cx)
    -> stdx::result<module_t, sema::diagnostics> {
    auto result{lower_module(mod, cx)};
    if (!result.diagnostics.empty()) { return stdx::err{std::move(result.diagnostics)}; }
    return std::move(result.uir);
}

} // namespace ghoti::uir
