#include <concepts>
#include <string>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::sema {

namespace {

// The identifier a source names: itself, a declaration's name, or a member access's member.
// `through_call` also takes a method call's member, which names what the call reaches
[[nodiscard]] auto named_identifier(const mod::module_t& m, const source& src, bool through_call)
    -> stdx::option<ast::node_id> {
    // A type written `m.T` names its member
    if (const auto type{src.as_opt<ast::explicit_type_id>()}) {
        const auto dot{m.ast.get_as_opt<ast::dot_expr>(*type)};
        return dot ? stdx::option<ast::node_id>{ast::node_id{dot->member}} : stdx::none;
    }

    const auto written{src.as_opt<ast::node_id>()};
    if (!written) { return stdx::none; }
    stdx::option<ast::node_id> node{*written};
    if (const auto call{m.ast.get_as_opt<ast::call_expr>(*node)}; call && through_call) {
        node = ast::node_id{call->function};
    }
    if (const auto stmt{m.ast.get_as_opt<ast::decl_stmt>(*node)}) {
        return ast::node_id{stmt->name};
    }
    if (const auto dot{m.ast.get_as_opt<ast::dot_expr>(*node)}) {
        return ast::node_id{dot->member};
    }
    if (m.ast.get_as_opt<ast::identifier_expr>(*node)) { return *node; }
    return stdx::none;
}

[[nodiscard]] auto name_span(const mod::module_t& m, ast::node_id name) -> located_span {
    return located_span{m.path, {m.ast.location_of(name), m.ast.end_location_of(name)}};
}

} // namespace

auto analyzer::note_type(inst_id inst, const operand& value) -> void {
    note_type_at(src_of(inst), value);
}

auto analyzer::note_type_at(const source& src, const operand& value) -> void {
    // Nothing hovers over a module, a generic, or a value still waiting for its type
    if (value.is<module_ref>() || value.is<generic_ref>() || value.is<anon_init>() ||
        value.is<cast_ref>() || value.is<implicit_call>() || value.is<slice_literal>() ||
        value.is<pack_ref>()) {
        return;
    }

    auto&      m{*u_.source};
    const auto name{named_identifier(m, src, false)};
    if (!name) { return; }

    // A generic body's first instance stands for the others
    DISCARD(m.set_sema_type_if(*name, type_of(value)));
}

auto analyzer::note_member(const source&                       src,
                           mod::module_t&                      owner,
                           ast::node_id                        name,
                           stdx::option<ast::node_id>          decl,
                           stdx::option<ast::explicit_type_id> annotation,
                           stdx::option<gs::type&>             type) -> void {
    if (!comp_.indexing()) { return; }
    auto&      m{*u_.source};
    const auto member{named_identifier(m, src, true)};
    if (!member) { return; }
    m.set_identifier_definition(*member, name_span(owner, name));
    m.add_identifier_position(*member);
    if (decl || annotation) {
        m.set_identifier_declaration(
            *member, gs::declaration_ref{.owner = &owner, .decl = decl, .annotation = annotation});
    }
    if (type) { DISCARD(m.set_sema_type_if(*member, *type)); }
}

auto analyzer::note_member_decl(const source& src, const unit& owner, decl_id decl) -> void {
    if (!comp_.indexing()) { return; }
    const auto& d{owner.uir->decl_at(decl)};
    if (!d.node) { return; }
    const auto stmt{owner.source->ast.get_as_opt<ast::decl_stmt>(*d.node)};
    if (!stmt) { return; }
    note_member(src, *owner.source, ast::node_id{stmt->name}, d.node, stdx::none, stdx::none);
}

auto analyzer::note_declared_name(ast::node_id name, gs::type& type) -> void {
    if (!comp_.indexing()) { return; }
    auto& m{*u_.source};
    m.add_identifier_position(name);
    DISCARD(m.set_sema_type_if(name, type));
}

auto analyzer::note_field(const source& src, gs::type& type, usize index) -> void {
    if (!comp_.indexing()) { return; }
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()};
        st && index < st->ast_fields.size()) {
        const auto& field{st->ast_fields[index]};
        note_member(src,
                    st->enclosing,
                    ast::node_id{field.name},
                    stdx::none,
                    field.explicit_type,
                    st->type_at(index));
    } else if (const auto ut{type.get_data().as_opt<gs::types::union_t>()};
               ut && index < ut->ast_fields.size()) {
        const auto& field{ut->ast_fields[index]};
        note_member(src,
                    ut->enclosing,
                    ast::node_id{field.name},
                    stdx::none,
                    field.explicit_type,
                    ut->type_at(index));
    }
}

} // namespace ghoti::uir::sema
