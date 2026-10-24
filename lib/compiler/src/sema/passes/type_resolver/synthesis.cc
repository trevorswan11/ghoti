#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::sema {

using namespace resolver_detail;

auto type_resolver::synthesize_ident(std::string_view name, bool is_public)
    -> ast::identifier_handle {
    auto storage{ctx_.arena.make_span<char>(name.size())};
    std::ranges::copy(name, storage.begin());
    const std::string_view stable{storage.data(), storage.size()};
    const syntax::token_t  tok{
        is_public ? syntax::token_type_t::PUBLIC : syntax::token_type_t::IDENT, stable};
    const auto id{resolving_.ast.add_node(tok, tok, ast::identifier_expr{stable})};
    resolving_.sync_side_tables_for_new_node();
    return ast::identifier_handle{id};
}

auto type_resolver::redirect_to_default_wrapper(ast::node_id          id,
                                                const ast::call_expr& call,
                                                usize                 written_args) -> bool {
    if (redirect_by_declaration(id, call, written_args)) { return true; }
    const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(call.function)};
    return dot && redirect_to_default_member(id, *dot, written_args);
}

auto type_resolver::redirect_to_default_member(ast::node_id         id,
                                               const ast::dot_expr& dot,
                                               usize                written_args) -> bool {
    auto&      ast{resolving_.ast};
    const auto method{ast.get_as_opt<ast::identifier_expr>(dot.member)};
    if (!method) { return false; }

    const auto            location{ast.location_of(dot.member)};
    const auto            stable{ast.intern(ast::default_wrapper_name(method->name, written_args))};
    const syntax::token_t tok{syntax::token_type_t::IDENT, stable, location.line, location.column};
    const ast::identifier_handle member{ast.add_node(tok, tok, ast::identifier_expr{stable})};
    resolving_.sync_side_tables_for_new_node();
    const ast::expr_handle callee{
        ast.add_node(tok, tok, ast::dot_expr{.object = dot.object, .member = member})};
    resolving_.sync_side_tables_for_new_node();

    // Resolved as a callee, the way `resolve_call` does, keeping quiet when there's no such member
    const auto diags_before{ctx_.diags.size()};
    {
        const mutating_context_guard callee_g{resolving_callee_, false};
        callee_dots_.insert(callee.get_index());
        const auto prev_callee{direct_callee_};
        direct_callee_ = member.get_index();
        resolve(callee);
        direct_callee_ = prev_callee;
        callee_dots_.erase(callee.get_index());
    }
    const auto found{last_type_.take()};
    if (!found || found->is_poison()) {
        DISCARD(ctx_.diags.split_off(diags_before));
        return false;
    }
    ast.get_as_mut<ast::call_expr>(id).function = callee;
    return true;
}

auto type_resolver::redirect_by_declaration(ast::node_id          id,
                                            const ast::call_expr& call,
                                            usize                 written_args) -> bool {
    auto&      ast{resolving_.ast};
    const auto dot{ast.get_as_opt<ast::dot_expr>(call.function)};
    const auto named{dot ? ast::node_id{dot->member} : ast::node_id{call.function}};
    if (!ast.get_as_opt<ast::identifier_expr>(named)) { return false; }

    // Only a call naming the declaration sees its defaults, through `const` aliases. A function
    // value has none. `object` is what reaches the declaring scope (`mod` in `mod.f`)
    stdx::option<ast::expr_handle> object;
    if (dot) { object.emplace(dot->object); }
    auto declaration{resolving_.get_identifier_declaration(named)};
    stdx::option<const ast::function_expr&> fn;
    std::string_view                        fn_name;
    // Each declaration is visited once, so a chain that loops back on itself ends
    ankerl::unordered_dense::set<gsl::not_null<const ast::decl_stmt*>> visited;
    while (!fn) {
        if (!declaration || !declaration->owner || !declaration->decl) { return false; }
        const auto& owner{*declaration->owner};
        const auto  decl{owner.ast.get_as_opt<ast::decl_stmt>(*declaration->decl)};
        if (!decl || !decl->value || !visited.insert(decl.get()).second) { return false; }
        if (const auto literal{owner.ast.get_as_opt<ast::function_expr>(*decl->value)}) {
            fn      = literal;
            fn_name = owner.ast.get_as<ast::identifier_expr>(decl->name).name;
            break;
        }

        // `const g = f;` / `const g = mod.f;`, whose value this module can name again
        if (!decl->has_modifier(ast::decl_modifiers::COMPTIME) ||
            decl->has_modifier(ast::decl_modifiers::MUT)) {
            return false;
        }
        if (owner.ast.get_as_opt<ast::identifier_expr>(*decl->value)) {
            if (!object && &owner != &resolving_) { return false; }
            declaration = owner.get_identifier_declaration(*decl->value);
        } else if (const auto alias_dot{owner.ast.get_as_opt<ast::dot_expr>(*decl->value)}) {
            if (object || &owner != &resolving_) { return false; }
            object.emplace(alias_dot->object);
            declaration = owner.get_identifier_declaration(alias_dot->member);
        } else {
            return false;
        }
    }
    if (!fn) { return false; }

    const auto first_default{std::ranges::find_if(
        fn->parameters, [](const auto& param) { return param.default_value.has_value(); })};
    if (first_default == fn->parameters.end() ||
        written_args < static_cast<usize>(first_default - fn->parameters.begin()) ||
        written_args >= fn->parameters.size()) {
        return false;
    }

    const auto            location{ast.location_of(named)};
    const auto            stable{ast.intern(ast::default_wrapper_name(fn_name, written_args))};
    const syntax::token_t tok{syntax::token_type_t::IDENT, stable, location.line, location.column};
    const ast::identifier_handle member{ast.add_node(tok, tok, ast::identifier_expr{stable})};
    resolving_.sync_side_tables_for_new_node();

    ast::expr_handle callee{member};
    if (object) {
        callee = ast::expr_handle{
            ast.add_node(tok, tok, ast::dot_expr{.object = *object, .member = member})};
        resolving_.sync_side_tables_for_new_node();
    }
    ast.get_as_mut<ast::call_expr>(id).function = callee;
    return true;
}

// Builds the `.<name> = <value>` accessor half of one synthesized initializer entry.
auto type_resolver::synthesize_implicit_access(std::string_view name)
    -> ast::implicit_access_handle {
    const auto            member{synthesize_ident(name, false)};
    const syntax::token_t tok{syntax::token_type_t::DOT, "."};
    const auto id{resolving_.ast.add_node(tok, tok, ast::implicit_access_expr{.member = member})};
    resolving_.sync_side_tables_for_new_node();
    return ast::implicit_access_handle{id};
}

auto type_resolver::synthesize_const_expr(const gir::const_value& val)
    -> stdx::option<ast::expr_handle> {
    if (const auto b{val.as_opt<bool>()}) {
        const syntax::token_t tok{
            *b ? syntax::token_type_t::BOOLEAN_TRUE : syntax::token_type_t::BOOLEAN_FALSE, "0"};
        const auto id{resolving_.ast.add_node(tok, tok, ast::bool_expr{})};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }
    if (const auto i{val.as_int_opt()}) {
        const syntax::token_t tok{syntax::token_type_t::INT_10, "0"};
        const auto            id{resolving_.ast.add_node(tok,
                                                         tok,
                                                         ast::int_literal_expr{
                                                             .value     = static_cast<u128>(*i),
                                                             .is_signed = true,
                                                             .spelling  = "0",
                                                         })};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }
    if (const auto f{val.as_opt<f128>()}) {
        const syntax::token_t tok{syntax::token_type_t::REAL, "0"};
        const auto            id{resolving_.ast.add_node(
            tok, tok, ast::float_literal_expr{.value = *f, .spelling = "0", .mantissa = {}})};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }
    if (const auto s{val.as_opt<std::string>()}) {
        auto storage{ctx_.arena.make_span<char>(s->size())};
        std::ranges::copy(*s, storage.begin());
        const std::string_view stable{storage.data(), storage.size()};
        const syntax::token_t  tok{syntax::token_type_t::STRING, stable};
        const auto             id{resolving_.ast.add_node(
            tok, tok, ast::string_expr{.value = stable, .spelling = stable})};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }

    const syntax::token_t init_tok{syntax::token_type_t::DOT, "."};

    if (const auto arr{val.as_opt<gir::const_array>()}) {
        ast::initializer_expr init{.object_type = stdx::none, .initializers = {}};
        init.initializers.reserve(arr->elements.size());
        for (const auto& elem : arr->elements) {
            const auto elem_expr{synthesize_const_expr(elem)};
            if (!elem_expr) { return stdx::none; }
            init.initializers.emplace_back<ast::initializer_expr::initializer>({
                .member = stdx::none,
                .value  = *elem_expr,
            });
        }
        const auto id{resolving_.ast.add_node(init_tok, init_tok, std::move(init))};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }

    if (const auto st{val.as_opt<gir::const_struct>()}) {
        ast::initializer_expr init{.object_type = stdx::none, .initializers = {}};
        init.initializers.reserve(st->fields.size());
        for (const auto& [name, field_val] : st->fields) {
            const auto field_expr{synthesize_const_expr(field_val)};
            if (!field_expr) { return stdx::none; }
            init.initializers.emplace_back<ast::initializer_expr::initializer>({
                .member = synthesize_implicit_access(name),
                .value  = *field_expr,
            });
        }
        const auto id{resolving_.ast.add_node(init_tok, init_tok, std::move(init))};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }

    if (const auto un{val.as_opt<gir::const_union>()}) {
        if (un->payload.empty()) { return stdx::none; }
        const auto payload_expr{synthesize_const_expr(un->payload.front())};
        if (!payload_expr) { return stdx::none; }
        ast::initializer_expr init{.object_type = stdx::none, .initializers = {}};
        init.initializers.emplace_back<ast::initializer_expr::initializer>({
            .member = synthesize_implicit_access(un->active_field),
            .value  = *payload_expr,
        });
        const auto id{resolving_.ast.add_node(init_tok, init_tok, std::move(init))};
        resolving_.sync_side_tables_for_new_node();
        return ast::expr_handle{id};
    }

    // A pointer nested inside an aggregate default (as opposed to the top-level `^opaque`
    // wrapper) isn't supported
    return stdx::none;
}

auto type_resolver::synthesize_enum(source_location loc, const gir::const_struct& desc)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    const auto field_err{
        [&](std::string_view what) -> stdx::result<gsl::not_null<type*>, diagnostic> {
            return make_sema_err(
                fmt::format("'@Enum': {}", what), error::COMPTIME_EVALUATION_FAILED, loc);
        }};

    const auto tag_v{read_desc_field<sema::type&>(desc, "tag_type")};
    const auto fields_arr{read_desc_field<gir::const_array>(desc, "fields")};
    const auto exhaustive_v{read_desc_field<bool>(desc, "exhaustive")};
    if (!tag_v) { return field_err("descriptor is missing 'tag_type'"); }
    if (!fields_arr) { return field_err("descriptor is missing 'fields'"); }
    if (!exhaustive_v) { return field_err("descriptor is missing 'exhaustive'"); }

    auto enumerations{
        make_uninit_span<ast::enum_expr::enumeration>(ctx_.arena, fields_arr->elements.size())};
    const auto scope_idx{ctx_.registry.create()};
    for (usize i{0}; i < fields_arr->elements.size(); ++i) {
        const auto fs{fields_arr->elements[i].as_opt<gir::const_struct>()};
        const auto name_v{fs ? read_desc_field<std::string>(*fs, "name") : stdx::none};
        const auto value_v{fs ? fs->get_field_opt("value") : stdx::none};
        const auto value_i{value_v ? value_v->as_int_opt() : stdx::none};
        if (!name_v || !value_i) {
            return field_err("every element of 'fields' needs a compile-time 'name' and 'value'");
        }

        const auto            name_ident{synthesize_ident(*name_v, false)};
        const syntax::token_t val_tok{syntax::token_type_t::INT_10, "0"};
        const auto            val_node{resolving_.ast.add_node(
            val_tok,
            val_tok,
            ast::int_literal_expr{
                .value = static_cast<u128>(*value_i), .is_signed = true, .spelling = "0"})};
        resolving_.sync_side_tables_for_new_node();
        new (&enumerations[i])
            ast::enum_expr::enumeration{.name = name_ident, .value = ast::expr_handle{val_node}};

        // The map key must outlive this function
        const auto stable_name{resolving_.ast.get_as<ast::identifier_expr>(name_ident).name};
        if (auto res{ctx_.registry.insert_into(
                scope_idx, resolving_, stable_name, symbols::enumeration{enumerations[i]})};
            !res) {
            return stdx::err<diagnostic>{std::move(res.error())};
        }
        // Ordinary `visit(enum_expr)` sets these once the variant is fully resolved. Nothing
        // else does this for a synthetic aggregate that never goes through that visitor.
        auto& variant_sym{ctx_.registry.get(scope_idx).get(stable_name)};
        variant_sym.set_kind(symbol_kind::VALUE);
        variant_sym.set_status(symbol_status::RESOLVED);
        resolving_.set_sema_type(name_ident, *tag_v);
    }

    auto& enum_type{*ctx_.pool[{type_kind::ENUM, types::mut::CONSTANT, scope_idx}]};
    enum_type.resolve_if<types::enum_t>(
        enumerations, !*exhaustive_v, *tag_v, gsl::span<stdx::option<type&>>{}, resolving_);
    enum_type.set_symbol_table_idx(scope_idx);
    return gsl::not_null{&enum_type};
}

auto type_resolver::synthesize_struct(source_location loc, const gir::const_struct& desc)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    const auto field_err{
        [&](std::string_view what) -> stdx::result<gsl::not_null<type*>, diagnostic> {
            return make_sema_err(
                fmt::format("'@Struct': {}", what), error::COMPTIME_EVALUATION_FAILED, loc);
        }};

    const auto fields_arr{read_desc_field<gir::const_array>(desc, "fields")};
    const auto is_extern_v{read_desc_field<bool>(desc, "is_extern")};
    const auto is_packed_v{read_desc_field<bool>(desc, "is_packed")};
    if (!fields_arr) { return field_err("descriptor is missing 'fields'"); }
    if (!is_extern_v) { return field_err("descriptor is missing 'is_extern'"); }
    if (!is_packed_v) { return field_err("descriptor is missing 'is_packed'"); }

    const auto n{fields_arr->elements.size()};
    auto       ast_fields{make_uninit_span<ast::struct_expr::field>(ctx_.arena, n)};
    auto       field_types{ctx_.pool.get_many_pending(n)};
    auto       field_alignments{ctx_.arena.make_span<u64>(n)};
    const auto scope_idx{ctx_.registry.create()};
    for (usize i{0}; i < n; ++i) {
        const auto fs{fields_arr->elements[i].as_opt<gir::const_struct>()};
        const auto name_v{fs ? read_desc_field<std::string>(*fs, "name") : stdx::none};
        const auto type_v{fs ? read_desc_field<sema::type&>(*fs, "type") : stdx::none};
        if (!name_v || !type_v) {
            return field_err("every element of 'fields' needs a compile-time 'name', 'type'");
        }
        field_types[i] = type_v;

        stdx::option<ast::expr_handle> default_value;
        const auto                     dv_field{fs->get_field_opt("default_value")};
        const auto dv_addr{dv_field ? dv_field->as_opt<gir::const_addr>() : stdx::none};
        if (dv_addr && !dv_addr->pointee.empty()) {
            const auto& dv{dv_addr->pointee.front()};
            if (const auto dt{dv.get_type()}; dt && !is_assignable(*dt, *type_v)) {
                return make_sema_err(
                    fmt::format("'@Struct': field '{}' default has type '{}', expected '{}'",
                                *name_v,
                                ctx_.type_display_name(*dt),
                                ctx_.type_display_name(*type_v)),
                    error::TYPE_MISMATCH,
                    loc);
            }
            const auto lit{synthesize_const_expr(dv)};
            if (!lit) {
                return make_sema_err(
                    fmt::format("'@Struct': field '{}' default value isn't constant-representable "
                                "(a pointer nested inside an aggregate default isn't supported)",
                                *name_v),
                    error::COMPTIME_EVALUATION_FAILED,
                    loc);
            }

            // A synthetic aggregate never goes through `visit(struct_expr)`, so nothing else
            // resolves this literal against the field's own type
            {
                const structural_guard inner_g{implicit_type_stack_, *type_v};
                resolve(*lit);
            }
            default_value.emplace(*lit);
        }

        field_alignments[i] = 0;
        if (const auto align_field{fs->get_field_opt("alignment")}) {
            const auto requested{align_field->as_uint_opt().value_or(0)};
            if (requested != 0 && (requested & (requested - 1)) != 0) {
                return field_err(fmt::format("field '{}' alignment {} is not a power of two",
                                             *name_v,
                                             static_cast<u64>(requested)));
            }
            field_alignments[i] = static_cast<u64>(requested);
        }

        const auto is_pub{read_desc_field<bool>(*fs, "is_pub").value_or(false)};
        const auto name_ident{synthesize_ident(*name_v, is_pub)};
        const auto ty_ident{synthesize_ident(ctx_.type_display_name(*type_v), false)};
        new (&ast_fields[i]) ast::struct_expr::field{
            .name          = name_ident,
            .explicit_type = ast::explicit_type_id{ast::explicit_type_kind::IDENT,
                                                   ast::type_modifier{},
                                                   syntax::token_type_t::IDENT,
                                                   static_cast<u64>((*ty_ident).get_index())},
            .default_value = default_value,
            .attributes    = stdx::none,
        };

        // The key must be the stable,  arena-backed name, not `*name_v`
        const auto stable_name{resolving_.ast.get_as<ast::identifier_expr>(name_ident).name};
        if (auto res{ctx_.registry.insert_into(
                scope_idx, resolving_, stable_name, symbols::struct_field{ast_fields[i]})};
            !res) {
            return stdx::err<diagnostic>{std::move(res.error())};
        }
        // Ordinary `visit(struct_expr)` sets these once the field is fully resolved. Nothing else
        // does this for a synthetic aggregate that never goes through that visitor.
        auto& field_sym{ctx_.registry.get(scope_idx).get(stable_name)};
        field_sym.set_kind(symbol_kind::VALUE);
        field_sym.set_status(symbol_status::RESOLVED);
    }
    auto& struct_type{*ctx_.pool[{type_kind::STRUCT, types::mut::CONSTANT, scope_idx}]};
    struct_type.resolve_if<types::struct_t>(field_types,
                                            ast_fields,
                                            gsl::span<stdx::option<type&>>{},
                                            resolving_,
                                            *is_extern_v,
                                            *is_packed_v,
                                            field_alignments);
    struct_type.set_symbol_table_idx(scope_idx);
    return gsl::not_null{&struct_type};
}

auto type_resolver::synthesize_union(source_location loc, const gir::const_struct& desc)
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    const auto field_err{
        [&](std::string_view what) -> stdx::result<gsl::not_null<type*>, diagnostic> {
            return make_sema_err(
                fmt::format("'@Union': {}", what), error::COMPTIME_EVALUATION_FAILED, loc);
        }};

    const auto fields_arr{read_desc_field<gir::const_array>(desc, "fields")};
    const auto is_extern_v{read_desc_field<bool>(desc, "is_extern")};
    const auto is_packed_v{read_desc_field<bool>(desc, "is_packed")};
    const auto tagged_v{read_desc_field<bool>(desc, "tagged")};
    if (!fields_arr) { return field_err("descriptor is missing 'fields'"); }
    if (!is_extern_v) { return field_err("descriptor is missing 'is_extern'"); }
    if (!is_packed_v) { return field_err("descriptor is missing 'is_packed'"); }
    if (!tagged_v) { return field_err("descriptor is missing 'tagged'"); }
    // `tag_type` defaults to `void`, so a descriptor without it is a `union(enum)`
    stdx::option<type&> tag_enum;
    if (const auto tag_v{read_desc_field<sema::type&>(desc, "tag_type")};
        tag_v && tag_v->get_kind() != type_kind::VOID_) {
        if (tag_v->get_kind() != type_kind::ENUM) {
            return field_err(fmt::format("'tag_type' must be an enum or void, found '{}'",
                                         ctx_.type_display_name(*tag_v)));
        }
        if (!*tagged_v || *is_extern_v || *is_packed_v) {
            return field_err("only a tagged Ghoti union can name a 'tag_type'");
        }
        tag_enum = tag_v;
    }

    const auto n{fields_arr->elements.size()};
    auto       ast_fields{make_uninit_span<ast::union_expr::field>(ctx_.arena, n)};
    auto       field_types{ctx_.pool.get_many_pending(n)};
    const auto scope_idx{ctx_.registry.create()};

    for (usize i{0}; i < n; ++i) {
        const auto fs{fields_arr->elements[i].as_opt<gir::const_struct>()};
        const auto name_v{fs ? read_desc_field<std::string>(*fs, "name") : stdx::none};
        const auto type_v{fs ? read_desc_field<sema::type&>(*fs, "type") : stdx::none};
        if (!name_v || !type_v) {
            return field_err("every element of 'fields' needs a compile-time 'name' and 'type'");
        }
        field_types[i] = type_v;

        const auto name_ident{synthesize_ident(*name_v, true)};
        const auto ty_ident{synthesize_ident(ctx_.type_display_name(*type_v), false)};
        new (&ast_fields[i]) ast::union_expr::field{
            .name          = name_ident,
            .explicit_type = ast::explicit_type_id{ast::explicit_type_kind::IDENT,
                                                   ast::type_modifier{},
                                                   syntax::token_type_t::IDENT,
                                                   static_cast<u64>((*ty_ident).get_index())},
            .attributes    = stdx::none,
        };

        const auto stable_name{resolving_.ast.get_as<ast::identifier_expr>(name_ident).name};
        if (auto res{ctx_.registry.insert_into(
                scope_idx, resolving_, stable_name, symbols::union_field{ast_fields[i]})};
            !res) {
            return stdx::err<diagnostic>{std::move(res.error())};
        }
        auto& field_sym{ctx_.registry.get(scope_idx).get(stable_name)};
        field_sym.set_kind(symbol_kind::VALUE);
        field_sym.set_status(symbol_status::RESOLVED);
    }

    if (tag_enum) {
        const auto& en{tag_enum->get_data().as<types::enum_t>()};
        const auto  names_variant{[&](std::string_view name) {
            return std::ranges::any_of(en.ast_enumerations, [&](const auto& e) {
                return en.enclosing.ast.get_as<ast::identifier_expr>(e.name).name == name;
            });
        }};
        for (usize i{0}; i < n; ++i) {
            const auto& name{resolving_.ast.get_as<ast::identifier_expr>(ast_fields[i].name).name};
            if (!names_variant(name)) {
                return field_err(fmt::format("field '{}' is not a variant of '{}'",
                                             name,
                                             ctx_.type_display_name(*tag_enum)));
            }
        }
        if (en.ast_enumerations.size() != n) {
            return field_err(fmt::format("every variant of '{}' needs a field",
                                         ctx_.type_display_name(*tag_enum)));
        }
    }

    auto& union_type{*ctx_.pool[{type_kind::UNION, types::mut::CONSTANT, scope_idx}]};
    union_type.resolve_if<types::union_t>(field_types,
                                          ast_fields,
                                          gsl::span<stdx::option<type&>>{},
                                          resolving_,
                                          !*tagged_v,
                                          *is_extern_v,
                                          *is_packed_v,
                                          tag_enum);
    union_type.set_symbol_table_idx(scope_idx);
    return gsl::not_null{&union_type};
}

auto type_resolver::known_length(ast::node_id expr) -> stdx::option<u64> {
    // An array carries its length in its type
    if (const auto expr_type{resolving_.get_sema_type_opt(expr)}) {
        gsl::not_null<type*> target{expr_type.get()};
        if (const auto ref{target->get_data().as_opt<types::reference>()}) {
            target = &ref->underlying;
        }
        target = &concrete_array_type(*target);
        if (const auto arr{target->get_data().as_opt<types::array>()}) { return arr->len; }
        if (const auto vec{vector_of(*target)}) { return vec->len; }
    }

    if (const auto str{resolving_.ast.get_as_opt<ast::string_expr>(expr)}) {
        return str->value.size();
    }

    if (const auto index{resolving_.ast.get_as_opt<ast::index_expr>(expr)}) {
        const auto range{resolving_.ast.get_as_opt<ast::range_expr>(index->index)};
        if (!range) { return stdx::none; }

        gir::const_eval evaluator{ctx_, resolving_};
        const auto      fold{[&](ast::expr_handle bound) -> stdx::option<i128> {
            const auto cv{evaluator.try_eval(bound)};
            if (!cv || cv->is_poison()) { return stdx::none; }
            return cv->as_int_opt();
        }};

        const auto         lo{range->lhs ? fold(*range->lhs) : stdx::option<i128>{0}};
        stdx::option<i128> hi;
        if (range->rhs) {
            const bool inclusive{(*index->index).get_token_type() ==
                                 syntax::token_type_t::DOT_DOT_EQ};
            hi = fold(*range->rhs).transform([&](i128 h) { return h + (inclusive ? 1 : 0); });
        } else if (const auto container_len{known_length(index->array)}) {
            hi = static_cast<i128>(*container_len);
        }
        if (!lo || !hi || *lo < 0 || *hi < *lo) { return stdx::none; }
        return static_cast<u64>(*hi - *lo);
    }

    // A `let` or `const` binding keeps the length of the expression it was initialized with
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)}) {
        const auto table_idx{resolving_.get_symbol_table_opt(expr)};
        if (!table_idx) { return stdx::none; }
        const auto sym{ctx_.registry.get(*table_idx).get_opt(ident->name)};
        if (!sym) { return stdx::none; }
        const auto node{sym->get_data().as_opt<symbols::node_t>()};
        if (!node) { return stdx::none; }
        const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
        if (!decl || !decl->value || decl->has_modifier(ast::decl_modifiers::MUT)) {
            return stdx::none;
        }
        return known_length(*decl->value);
    }

    return stdx::none;
}

} // namespace ghoti::sema
