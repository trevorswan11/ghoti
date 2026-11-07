#include <string_view>
#include <utility>
#include <vector>

#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "astgen_internal.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir::detail {

namespace {

// A function literal marked `@[testing]`, directly or through the declaration it initializes
[[nodiscard]] auto is_testing_fn(const ast::function_expr&           fn,
                                 stdx::option<const ast::decl_stmt&> decl) -> bool {
    if (fn.attributes && fn.attributes->find(ast::attribute_kind::TESTING)) { return true; }
    return decl && decl->attributes && decl->attributes->find(ast::attribute_kind::TESTING);
}

[[nodiscard]] auto pointer_flags(ast::type_modifier modifier) -> u32 {
    u32 out{0};
    if (modifier.is_mutable_ref() || modifier.is_mutable_ptr()) { out |= flags::MUT; }
    if (modifier.is_poly()) { out |= flags::POLY | flags::MUT; }
    if (modifier.is_volatile()) { out |= flags::VOLATILE | flags::MUT; }
    return out;
}

} // namespace

auto astgen::type(ast::explicit_type_id id) -> ref {
    const auto inner{type_inner(id)};
    const auto modifier{id.get_modifier()};
    if (modifier.is_value()) { return inner; }
    const auto t{modifier.is_ref() ? tag::ref_type : tag::ptr_type};
    return add_pl(t, writer().ref_field(inner).u32_field(pointer_flags(modifier)), id);
}

auto astgen::type_inner(ast::explicit_type_id id) -> ref {
    using ast::explicit_type_kind;
    switch (id.get_kind()) {
    case explicit_type_kind::IDENT: {
        const auto& ident{ast_.get_as<ast::identifier_expr>(id)};
        if (const auto prim{primitive_named(ident.name, id.get_token_type(), id)}) { return *prim; }
        if (const auto builtin{id.get_builtin()}) {
            return add_un(tag::builtin_ref, {}, static_cast<u32>(*builtin), id);
        }
        return identifier(id, ast_.location_of(id), ident.name, use::VALUE).value;
    }
    case explicit_type_kind::DOT: {
        const auto& dot{ast_.get_as<ast::dot_expr>(id)};
        return add_pl(tag::field_val,
                      writer().ref_field(expr(dot.object)).str_field(name_of(dot.member)),
                      id);
    }
    case explicit_type_kind::CALL:
        return call(id, ast_.location_of(id), ast_.get_as<ast::call_expr>(id));
    case explicit_type_kind::FUNCTION:
        return fn_type(id, ast_.get_as<ast::explicit_function_type>(id));
    case explicit_type_kind::RECURSIVE: return type(ast_.get_as<ast::explicit_type_id>(id));
    case explicit_type_kind::STRUCT:    return struct_decl(id, ast_.get_as<ast::struct_expr>(id));
    case explicit_type_kind::ENUM:      return enum_decl(id, ast_.get_as<ast::enum_expr>(id));
    case explicit_type_kind::UNION:     return union_decl(id, ast_.get_as<ast::union_expr>(id));
    case explicit_type_kind::INTERFACE:
        return interface_decl(id, ast_.get_as<ast::interface_expr>(id));
    case explicit_type_kind::ARRAY: {
        const auto& array{ast_.get_as<ast::explicit_array_type>(id)};
        u32         bits{0};
        if (array.mut_elements) { bits |= flags::MUT; }
        if (array.null_terminated) { bits |= flags::SENTINEL; }
        if (array.poly_elements) { bits |= flags::POLY; }
        const auto len{array.dimension ? expr(*array.dimension) : ref{}};
        const auto elem{type(array.inner_explicit_type)};
        if (!array.dimension) {
            return add_pl(tag::slice_type, writer().ref_field(elem).u32_field(bits), id);
        }
        return add_pl(tag::array_type, writer().ref_field(len).ref_field(elem).u32_field(bits), id);
    }
    case explicit_type_kind::DYN: {
        const auto&                                   dyn{ast_.get_as<ast::explicit_dyn_type>(id)};
        const auto                                    iface{type(dyn.interface_type)};
        std::vector<std::pair<std::string_view, ref>> bindings;
        for (const auto& binding : dyn.assoc_bindings) {
            bindings.emplace_back(name_of(binding.name), type(binding.type));
        }
        auto record{writer()};
        record.ref_field(iface).count(bindings.size());
        for (const auto& [name, bound] : bindings) { record.str_field(name).ref_field(bound); }
        return add_pl(tag::dyn_type, record, id);
    }
    case explicit_type_kind::EXPR: return expr(ast_.get_as<ast::explicit_expr_type>(id).expr);
    }
    UNREACHABLE("every explicit type kind has a lowering");
}

auto astgen::type_body(ast::explicit_type_id id) -> std::vector<u32> {
    std::vector<u32> body;
    const body_guard guard{*this, body};
    yields(type(id), id);
    return body;
}

auto astgen::fn_type(ast::explicit_type_id id, const ast::explicit_function_type& fn) -> ref {
    std::vector<ref> params;
    for (const auto param : fn.parameter_types) { params.push_back(type(param)); }
    const auto ret{type(fn.explicit_return_type)};
    u32        bits{0};
    if (fn.variadic) { bits |= flags::VARIADIC; }
    if (fn.is_extern) { bits |= flags::EXTERN_FN; }
    if (fn.is_dyn_fn) { bits |= flags::DYN_FN; }
    if (fn.is_impl_fn) { bits |= flags::IMPL_FN; }
    if (fn.has_explicit_conv) { bits |= flags::EXPLICIT_CONV; }

    auto record{writer()};
    record.refs_field(params).count(fn.parameter_names.size());
    for (const auto name : fn.parameter_names) { record.str_field(name_of(name)); }
    record.ref_field(ret).u32_field(static_cast<u32>(fn.conv)).u32_field(bits);
    return add_pl(tag::fn_type, record, id);
}

auto astgen::begin_aggregate(source src) -> u32 {
    auto& added{push_scope(scope_kind::NAMESPACE)};
    added.is_boundary  = true;
    added.is_aggregate = true;
    added.type_name    = std::exchange(pending_type_name_, {});
    return reserve(src);
}

auto astgen::write_captures(record_writer&                   record,
                            const std::vector<capture_slot>& captures,
                            const std::vector<u32>&          outer) -> void {
    record.count(captures.size());
    for (usize i{0}; i < captures.size(); ++i) {
        record.ref_field(static_cast<ref>(outer[i]))
            .u32_field(captures[i].by_ref ? 1U : 0U)
            .str_field(captures[i].name);
    }
}

auto astgen::finish_aggregate(source src)
    -> std::pair<std::vector<capture_slot>, std::vector<u32>> {
    auto captures{scopes_.back()->captures};
    auto copy{std::move(*scopes_.back())};
    pop_scope();
    return {captures, outer_captures(copy, src)};
}

auto astgen::struct_decl(source src, const ast::struct_expr& node) -> ref {
    const auto inst{begin_aggregate(src)};
    const auto fields{spliced_group(node.fields, node.cfg_groups)};
    const auto members{spliced_group(node.members, node.member_cfg_groups)};
    for (const auto& field : fields) {
        DISCARD(declare_member(
            name_of(field.name), location_of(field.name), {.kind = binding_kind::FIELD}));
    }

    struct field_record {
        std::string_view name{};
        u32              bits{0};
        std::vector<u32> type{};
        std::vector<u32> default_value{};
        std::vector<u32> attrs{};
    };
    std::vector<field_record> lowered_fields;
    const auto                decls{namespace_members(inst, members)};
    for (const auto& field : fields) {
        field_record record{.name = name_of(field.name),
                            .bits = field.is_public() ? flags::PUBLIC : 0U};
        functions_.emplace_back();
        record.type = type_body(field.explicit_type);
        if (field.default_value) { record.default_value = value_body(*field.default_value); }
        lower_attributes(field.attributes, stdx::none, record.attrs);
        functions_.pop_back();
        lowered_fields.push_back(std::move(record));
    }
    const auto [captures, outer]{finish_aggregate(src)};

    u32 bits{0};
    if (node.is_extern) { bits |= flags::AGG_EXTERN; }
    if (node.is_packed) { bits |= flags::AGG_PACKED; }
    auto record{writer()};
    record.u32_field(bits);
    write_captures(record, captures, outer);
    record.count(lowered_fields.size());
    for (const auto& field : lowered_fields) {
        record.str_field(field.name)
            .u32_field(field.bits)
            .body_field(field.type)
            .body_field(field.default_value)
            .body_field(field.attrs);
    }
    record.count(decls.size());
    for (const auto d : decls) { record.u32_field(d); }
    fill_pl(inst, tag::struct_decl, record);
    return inst_ref(inst);
}

auto astgen::union_decl(source src, const ast::union_expr& node) -> ref {
    // The tag names a type from the enclosing scope, not one of the union's own members
    std::vector<u32> tag_type;
    if (node.tag_type) { tag_type = type_body(*node.tag_type); }

    const auto inst{begin_aggregate(src)};
    const auto fields{spliced_group(node.fields, node.cfg_groups)};
    const auto members{spliced_group(node.members, node.member_cfg_groups)};
    for (const auto& field : fields) {
        DISCARD(declare_member(
            name_of(field.name), location_of(field.name), {.kind = binding_kind::FIELD}));
    }

    struct field_record {
        std::string_view name{};
        std::vector<u32> type{};
        std::vector<u32> attrs{};
    };
    std::vector<field_record> lowered_fields;
    const auto                decls{namespace_members(inst, members)};
    for (const auto& field : fields) {
        field_record record{.name = name_of(field.name)};
        functions_.emplace_back();
        record.type = type_body(field.explicit_type);
        lower_attributes(field.attributes, stdx::none, record.attrs);
        functions_.pop_back();
        lowered_fields.push_back(std::move(record));
    }
    const auto [captures, outer]{finish_aggregate(src)};

    u32 bits{0};
    if (node.is_extern) { bits |= flags::AGG_EXTERN; }
    if (node.is_packed) { bits |= flags::AGG_PACKED; }
    auto record{writer()};
    record.u32_field(bits).u32_field(static_cast<u32>(node.tag)).body_field(tag_type);
    write_captures(record, captures, outer);
    record.count(lowered_fields.size());
    for (const auto& field : lowered_fields) {
        record.str_field(field.name).body_field(field.type).body_field(field.attrs);
    }
    record.count(decls.size());
    for (const auto d : decls) { record.u32_field(d); }
    fill_pl(inst, tag::union_decl, record);
    return inst_ref(inst);
}

auto astgen::enum_decl(source src, const ast::enum_expr& node) -> ref {
    std::vector<u32> underlying;
    if (node.underlying) { underlying = type_body(*node.underlying); }

    const auto inst{begin_aggregate(src)};
    const auto values{spliced_group(node.enumerations, node.cfg_groups)};
    const auto members{spliced_group(node.members, node.member_cfg_groups)};
    for (const auto& value : values) {
        DISCARD(declare_member(
            name_of(value.name), location_of(value.name), {.kind = binding_kind::FIELD}));
    }

    std::vector<std::pair<std::string_view, std::vector<u32>>> lowered_values;
    const auto decls{namespace_members(inst, members)};
    for (const auto& value : values) {
        functions_.emplace_back();
        lowered_values.emplace_back(name_of(value.name),
                                    value.value ? value_body(*value.value) : std::vector<u32>{});
        functions_.pop_back();
    }
    const auto [captures, outer]{finish_aggregate(src)};

    auto record{writer()};
    record.u32_field(node.non_exhaustive ? flags::NON_EXHAUSTIVE : 0U).body_field(underlying);
    write_captures(record, captures, outer);
    record.count(lowered_values.size());
    for (const auto& [name, body] : lowered_values) { record.str_field(name).body_field(body); }
    record.count(decls.size());
    for (const auto d : decls) { record.u32_field(d); }
    fill_pl(inst, tag::enum_decl, record);
    return inst_ref(inst);
}

auto astgen::interface_decl(source src, const ast::interface_expr& node) -> ref {
    const auto inst{begin_aggregate(src)};
    functions_.emplace_back();
    const auto pop_state{gsl::finally([this] { functions_.pop_back(); })};

    // Associated types and constants are names method signatures and default bodies can read
    std::vector<u32> header;
    {
        const body_guard guard{*this, header};
        u32              slot{0};
        const auto       bind_assoc{[&](ast::identifier_handle name) {
            const auto bound{
                add_pl(tag::bind,
                       writer().str_field(name_of(name)).u32_field(slot++).u32_field(0),
                       ast::node_id{name})};
            DISCARD(declare_member(
                name_of(name), location_of(name), {.kind = binding_kind::VALUE, .value = bound}));
        }};
        for (const auto& assoc : node.assoc_types) { bind_assoc(assoc.name); }
        for (const auto& assoc : node.assoc_consts) { bind_assoc(assoc.name); }
    }

    struct assoc_record {
        std::string_view name{};
        std::vector<u32> type{};
        std::vector<u32> default_value{};
    };
    std::vector<assoc_record> types;
    for (const auto& assoc : node.assoc_types) {
        types.push_back({name_of(assoc.name),
                         type_body(assoc.annotation),
                         assoc.default_type ? type_body(*assoc.default_type) : std::vector<u32>{}});
    }
    std::vector<assoc_record> consts;
    for (const auto& assoc : node.assoc_consts) {
        consts.push_back(
            {name_of(assoc.name),
             type_body(assoc.explicit_type),
             assoc.default_value ? value_body(*assoc.default_value) : std::vector<u32>{}});
    }

    struct method_record {
        std::string_view name{};
        u32              bits{0};
        std::vector<u32> signature{};
        std::vector<u32> attrs{};
    };
    std::vector<method_record> methods;
    for (const auto& method : node.methods) {
        method_record record{.name = name_of(method.name),
                             .bits = method.is_public() ? flags::PUBLIC : 0U};
        {
            const body_guard guard{*this, record.signature};
            yields(lower_function(ast::node_id{method.signature},
                                  ast_.get_as<ast::function_expr>(method.signature)),
                   ast::node_id{method.signature});
        }
        lower_attributes(method.attributes, stdx::none, record.attrs);
        methods.push_back(std::move(record));
    }
    const auto [captures, outer]{finish_aggregate(src)};

    auto record{writer()};
    write_captures(record, captures, outer);
    record.body_field(header);
    for (const auto* list : {&types, &consts}) {
        record.count(list->size());
        for (const auto& assoc : *list) {
            record.str_field(assoc.name).body_field(assoc.type).body_field(assoc.default_value);
        }
    }
    record.count(methods.size());
    for (const auto& method : methods) {
        record.str_field(method.name)
            .u32_field(method.bits)
            .body_field(method.signature)
            .body_field(method.attrs);
    }
    fill_pl(inst, tag::interface_decl, record);
    return inst_ref(inst);
}

auto astgen::namespace_members(u32 inst, const ast::member_list& members) -> std::vector<u32> {
    std::vector<u32>                                   decls;
    std::vector<std::pair<u32, const ast::decl_stmt*>> bodies;
    for (const auto member : members) {
        const ast::node_id id{member};
        if (const auto decl{ast_.get_as_opt<ast::decl_stmt>(id)}) {
            check_decl_modifiers(id, *decl, false);
            const auto index{add_value_decl(*decl, id, inst)};
            decls.push_back(index);
            bodies.emplace_back(index, decl.get());
            DISCARD(declare_member(name_of(decl->name),
                                   location_of(decl->name),
                                   {.kind         = binding_kind::DECL,
                                    .decl         = index,
                                    .mutable_decl = decl->has_modifier(ast::decl_modifiers::MUT)}));
            continue;
        }
        const auto& import{ast_.get_as<ast::import_stmt>(id)};
        const auto  index{add_import_decl(id, import, inst)};
        decls.push_back(index);
        if (const auto named{import.get_name(ast_)}) {
            DISCARD(declare_member(named->second,
                                   import_location(id, import),
                                   {.kind = binding_kind::DECL, .decl = index}));
        }
    }
    for (const auto& [index, decl] : bodies) { lower_decl_bodies(index, *decl); }
    out_.add_namespace({.owner = inst, .decls = decls});
    return decls;
}

auto astgen::lower_param(const ast::function_expr::parameter& param, u32 extra_flags, bool bodyless)
    -> ref {
    // The type comes first, so a parameter can't name itself in it
    const auto type_insts{param.explicit_type ? type_body(*param.explicit_type)
                                              : std::vector<u32>{}};
    const auto default_insts{param.default_value ? value_body(*param.default_value)
                                                 : std::vector<u32>{}};
    const auto name{ast_.get_as_opt<ast::identifier_expr>(param.name)};

    u32 bits{extra_flags};
    if (param.is_comptime || param.is_comptime_written) { bits |= flags::PARAM_COMPTIME; }
    if (param.is_comptime_written) { bits |= flags::PARAM_COMPTIME_WRITTEN; }
    if (param.is_pack) { bits |= flags::PARAM_PACK; }
    if (param.is_written) { bits |= flags::PARAM_WRITTEN; }
    if (!name) { bits |= flags::PARAM_DISCARDED; }

    const auto added{add_pl(tag::param,
                            writer()
                                .str_field(name ? name->name : std::string_view{})
                                .body_field(type_insts)
                                .body_field(default_insts)
                                .u32_field(bits)
                                .u32_field(0),
                            ast::node_id{param.name})};
    if (name) {
        DISCARD(declare(name->name,
                        location_of(param.name),
                        {.kind = binding_kind::VALUE, .value = added},
                        bodyless));
    }
    return added;
}

auto astgen::lower_function(ast::node_id                        id,
                            const ast::function_expr&           fn,
                            stdx::option<const ast::decl_stmt&> decl) -> ref {
    auto& boundary{push_scope(scope_kind::FUNCTION)};
    boundary.moves   = fn.is_move;
    boundary.mutated = mutated_within(id);
    functions_.push_back(
        {.allows_return = true, .allows_defer = true, .testing = is_testing_fn(fn, decl)});
    // A signature without a body binds its parameter names for dependent types alone
    const bool bodyless{!fn.body};

    std::vector<u32>                              params;
    std::vector<std::pair<u32, std::vector<ref>>> impl_bounds;
    {
        const body_guard guard{*this, params};
        if (fn.self) {
            const auto name{name_of(fn.self->name)};
            const auto added{add_pl(tag::param,
                                    writer()
                                        .str_field(name)
                                        .body_field({})
                                        .body_field({})
                                        .u32_field(flags::PARAM_SELF)
                                        .u32_field(static_cast<u32>(fn.self->modifier.get_raw())),
                                    ast::node_id{fn.self->name})};
            DISCARD(declare(name,
                            location_of(fn.self->name),
                            {.kind = binding_kind::VALUE, .value = added},
                            bodyless));
        }
        for (const auto& param : fn.parameters) { DISCARD(lower_param(param, 0, bodyless)); }
        for (const auto& bound : fn.impl_bounds) {
            std::vector<ref> interfaces;
            for (const auto iface : bound.interfaces) { interfaces.push_back(type(iface)); }
            impl_bounds.emplace_back(bound.param_index, std::move(interfaces));
        }
    }

    const auto       ret{type_body(fn.explicit_return_type)};
    std::vector<u32> attrs;
    lower_attributes(fn.attributes, stdx::none, attrs);
    if (decl) { lower_attributes(decl->attributes, true, attrs); }

    std::vector<u32> body;
    if (fn.body) {
        const auto& block{ast_.get_as<ast::block_stmt>(*fn.body)};
        const auto  statements{spliced(block.statements)};
        announce_later(statements);
        const body_guard guard{*this, body};
        if (!block_statements(statements)) { emit_defers(0, false, {}); }
    }

    functions_.pop_back();
    auto captures{scopes_.back()->captures};
    auto copy{std::move(*scopes_.back())};
    pop_scope();

    u32 bits{0};
    if (fn.variadic) { bits |= flags::VARIADIC; }
    if (fn.is_move) { bits |= flags::MOVE; }
    if (fn.is_extern) { bits |= flags::EXTERN_FN; }
    if (fn.is_type_expr) { bits |= flags::TYPE_EXPR; }
    if (fn.has_explicit_conv) { bits |= flags::EXPLICIT_CONV; }
    if (is_testing_fn(fn, decl)) { bits |= flags::TESTING; }

    auto record{writer()};
    record.body_field(params)
        .body_field(ret)
        .body_field(body)
        .u32_field(static_cast<u32>(fn.conv))
        .u32_field(bits)
        .body_field(attrs)
        .count(impl_bounds.size());
    for (const auto& [param, interfaces] : impl_bounds) {
        record.u32_field(param).refs_field(interfaces);
    }
    const auto func{add_pl(tag::func, record, id)};
    if (captures.empty()) { return func; }

    const auto outer{outer_captures(copy, id)};
    auto       closure{writer()};
    closure.ref_field(func);
    write_captures(closure, captures, outer);
    return add_pl(tag::closure, closure, id);
}

} // namespace ghoti::uir::detail
