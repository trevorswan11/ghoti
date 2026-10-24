#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <type_traits>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"

// Performs the resolution and poison check & bubble & return operation
#define TRY_RESOLVE(resolvable_expr)                                                       \
    do {                                                                                   \
        resolve(resolvable_expr);                                                          \
        if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); } \
    } while (false)

#define TRY_DESC_FIELD(out_var, T, field_name)                 \
    const auto out_var{read_desc_field<T>(*desc, field_name)}; \
    if (!out_var) { return field_err(field_name); }

// File-scope helpers shared by more than one of the type resolver's sources
namespace ghoti::sema::resolver_detail {

// A range is only legal as the whole subscript or `for` iterable, not nested inside one
template <typename ID> [[nodiscard]] auto is_range(const ID& id) -> bool {
    return id.template is<ast::range_expr>();
}

[[nodiscard]] inline auto callconv_requires_extern(const source_location& location) -> diagnostic {
    return diagnostic{"`callconv(...)` only applies to a thin `extern fn(...)` type; an erased "
                      "`fn(...)` callable has no C calling convention",
                      error::CALLCONV_REQUIRES_EXTERN_FN,
                      location};
}

[[nodiscard]] inline auto incomplete_array_item(const source_location& location) -> diagnostic {
    return diagnostic{
        "Array elements cannot have an incomplete type", error::CYCLIC_DEPENDENCY, location};
}

[[nodiscard]] constexpr auto array_element_mutability(bool mut_elements,
                                                      bool poly_elements) noexcept
    -> types::mutability_modifiers {
    if (poly_elements) { return types::mut::POLY; }
    return mut_elements ? types::mut::MUTABLE : types::mut::CONSTANT;
}

[[nodiscard]] inline auto container_element_mutability(const type& t)
    -> types::mutability_modifiers {
    if (const auto def{t.get_data().as_opt<types::deferred_array>()}) {
        return array_element_mutability(def->array.mut_elements, def->array.poly_elements);
    }
    if (t.is_poly()) { return types::mut::POLY; }
    return t.is_constant() ? types::mut::CONSTANT : types::mut::MUTABLE;
}

// Reads one field off a compile-time-known descriptor struct
template <typename T>
[[nodiscard]] auto read_desc_field(const gir::const_struct& desc, std::string_view name)
    -> stdx::option<T> {
    const auto field{desc.get_field_opt(name)};
    if (!field) { return stdx::none; }
    if constexpr (std::is_same_v<T, u64>) {
        return field->as_u64_opt();
    } else if constexpr (std::is_same_v<T, sema::type&>) {
        const auto v{field->as_opt<stdx::option<sema::type&>>()};
        if (!v || !*v) { return stdx::none; }
        return **v;
    } else {
        return field->as_opt<T>().materialize();
    }
}

// Whether an annotation spells `mut?` itself (`&mut? T`, `^mut? T`, `[]mut? T`), as opposed to
// naming a type that merely contains one
[[nodiscard]] inline auto spells_poly(const ast::AST& tree, stdx::option<ast::explicit_type_id> t)
    -> bool {
    if (!t) { return false; }
    if (t->get_modifier().is_poly()) { return true; }
    switch (t->get_kind()) {
    case ast::explicit_type_kind::RECURSIVE:
        return spells_poly(tree, tree.get_as<ast::explicit_type_id>(*t));
    case ast::explicit_type_kind::ARRAY: {
        const auto& array{tree.get_as<ast::explicit_array_type>(*t)};
        return array.poly_elements || spells_poly(tree, array.inner_explicit_type);
    }
    default: return false;
    }
}

[[nodiscard]] inline auto is_lvalue_expression(const mod::module_t& module,
                                               ast::expr_handle     expr) noexcept -> bool {
    if (const auto ty{module.get_sema_type_opt(expr)}) {
        if (ty->get_kind() == type_kind::REFERENCE) { return true; }
    }
    if (module.ast.get_as_opt<ast::identifier_expr>(expr)) { return true; }
    if (const auto dot{module.ast.get_as_opt<ast::dot_expr>(expr)}) {
        // `@This().member` reaches a static member through the enclosing type itself
        if (const auto call{module.ast.get_as_opt<ast::call_expr>(dot->object)};
            call && call->function->get_builtin() == syntax::builtin_t::THIS) {
            return true;
        }
        if (const auto obj_t{module.get_sema_type_opt(dot->object)}) {
            if (obj_t->get_kind() == type_kind::POINTER ||
                obj_t->get_kind() == type_kind::REFERENCE) {
                return true;
            }
        }
        return is_lvalue_expression(module, dot->object);
    }

    if (const auto idx{module.ast.get_as_opt<ast::index_expr>(expr)}) {
        if (const auto arr_t{module.get_sema_type_opt(idx->array)}) {
            if (arr_t->get_kind() == type_kind::POINTER ||
                arr_t->get_kind() == type_kind::REFERENCE ||
                arr_t->get_kind() == type_kind::SLICE) {
                return true;
            }
        }
        return is_lvalue_expression(module, idx->array);
    }
    if (module.ast.get_as_opt<ast::dereference_expr>(expr)) { return true; }
    // `@field(x, "name")` names the field itself, so it is as assignable as `x.name`
    if (const auto call{module.ast.get_as_opt<ast::call_expr>(expr)}) {
        return call->function->get_builtin() == syntax::builtin_t::FIELD;
    }
    return false;
}

// What an atomic builtin's argument is, which decides the type it's resolved against
enum class atomic_arg : u8 {
    PLAIN,
    OPERAND,
    ORDER,
    OP,
};

// The roles of an atomic builtin's arguments, in order. Empty for any other builtin
[[nodiscard]] inline auto atomic_arg_roles(stdx::option<syntax::builtin_t> builtin) noexcept
    -> gsl::span<const atomic_arg> {
    using enum atomic_arg;
    static constexpr std::array load{PLAIN, PLAIN, ORDER};
    static constexpr std::array store{PLAIN, OPERAND, ORDER};
    static constexpr std::array rmw{PLAIN, PLAIN, OP, OPERAND, ORDER};
    static constexpr std::array cmpxchg{PLAIN, PLAIN, OPERAND, OPERAND, ORDER, ORDER, PLAIN};
    static constexpr std::array fence{ORDER};
    if (!builtin) { return {}; }
    switch (*builtin) {
    case syntax::builtin_t::ATOMIC_LOAD:    return load;
    case syntax::builtin_t::ATOMIC_STORE:   return store;
    case syntax::builtin_t::ATOMIC_RMW:     return rmw;
    case syntax::builtin_t::CMPXCHG_WEAK:
    case syntax::builtin_t::CMPXCHG_STRONG: return cmpxchg;
    case syntax::builtin_t::FENCE:          return fence;
    default:                                return {};
    }
}

[[nodiscard]] inline auto any_param_comptime(const ast::function_expr& fn) noexcept -> bool {
    return std::ranges::any_of(fn.parameters, [](const auto& p) { return p.is_comptime; });
}

// The earlier parameter whose name a bare `x: T` annotation at `idx` spells, binding `T`
[[nodiscard]] inline auto bound_type_param(const ast::AST&                                tree,
                                           gsl::span<const ast::function_expr::parameter> params,
                                           usize idx) -> stdx::option<usize> {
    if (idx >= params.size() || !params[idx].explicit_type) { return stdx::none; }
    // `^T` and `&T` keep their modifier in the id, with `T` as the payload
    if (!params[idx].explicit_type->get_modifier().is_value()) { return stdx::none; }
    const auto ident{tree.get_as_opt<ast::identifier_expr>(*params[idx].explicit_type)};
    if (!ident) { return stdx::none; }
    for (usize k{0}; k < idx; ++k) {
        if (!params[k].name.is<ast::identifier_expr>()) { continue; }
        if (tree.get_as<ast::identifier_expr>(params[k].name).name == ident->name) { return k; }
    }
    return stdx::none;
}

// The earlier parameter a `y: @TypeOf(x)` parameter's type names
[[nodiscard]] inline auto typeof_bound_param(const ast::AST&                                tree,
                                             gsl::span<const ast::function_expr::parameter> params,
                                             usize idx) -> stdx::option<usize> {
    if (idx >= params.size() || !params[idx].explicit_type) { return stdx::none; }
    if (!params[idx].explicit_type->get_modifier().is_value()) { return stdx::none; }
    const auto call{tree.get_as_opt<ast::call_expr>(*params[idx].explicit_type)};
    if (!call || call->function->get_builtin() != syntax::builtin_t::TYPE_OF ||
        call->arguments.size() != 1) {
        return stdx::none;
    }
    const auto arg{call->arguments.front().as_opt<ast::expr_handle>()};
    const auto ident{arg ? tree.get_as_opt<ast::identifier_expr>(*arg) : stdx::none};
    if (!ident) { return stdx::none; }
    for (usize k{0}; k < idx; ++k) {
        if (!params[k].name.is<ast::identifier_expr>()) { continue; }
        if (tree.get_as<ast::identifier_expr>(params[k].name).name == ident->name) { return k; }
    }
    return stdx::none;
}

// A bare `auto` parameter, which keeps a `comptime_int`/`comptime_float` argument's type
[[nodiscard]] inline auto
keeps_comptime_numeric(const ast::function_expr::parameter& param) noexcept -> bool {
    return !param.is_pack && param.explicit_type &&
           param.explicit_type->get_token_type() == syntax::token_type_t::AUTO_TYPE;
}

// Whether the parameter at `idx` is implicitly `comptime` given its argument types: one typed by
// its argument (`auto`, `@TypeOf(x)`, `T`) that got a `comptime_int`/`comptime_float`, which only
// exists at compile time
[[nodiscard]] inline auto binds_implicit_comptime(const ast::AST&                       tree,
                                                  const ast::function_expr&             fn,
                                                  usize                                 idx,
                                                  gsl::span<const gsl::not_null<type*>> arg_types)
    -> bool {
    const auto& param{fn.parameters[idx]};
    if (param.is_comptime || param.is_pack || idx >= arg_types.size() ||
        !is_comptime_numeric(arg_types[idx]->get_kind())) {
        return false;
    }
    return keeps_comptime_numeric(param) || typeof_bound_param(tree, fn.parameters, idx) ||
           bound_type_param(tree, fn.parameters, idx);
}

// `@splat`, `@reduce`, and the other builtins that work on a vector's lanes
[[nodiscard]] inline auto is_lane_builtin(stdx::option<syntax::builtin_t> builtin) -> bool {
    if (!builtin) { return false; }
    switch (*builtin) {
    case syntax::builtin_t::SPLAT:
    case syntax::builtin_t::REDUCE:
    case syntax::builtin_t::SHUFFLE:
    case syntax::builtin_t::SELECT:
    case syntax::builtin_t::MASKED_LOAD:
    case syntax::builtin_t::MASKED_STORE:
    case syntax::builtin_t::GATHER:
    case syntax::builtin_t::SCATTER:      return true;
    default:                              return false;
    }
}

// Why an unmarked parameter is `comptime`: its function reads it at compile time somewhere
[[nodiscard]] inline auto inferred_comptime_reason(const mod::module_t&                 owner,
                                                   const ast::function_expr::parameter& param)
    -> std::string {
    if (!param.comptime_read) {
        return "; the parameter is compile-time because this function reads it in a "
               "compile-time position";
    }
    const auto name{owner.ast.get_as<ast::identifier_expr>(*param.name).name};
    return fmt::format("; '{}' is compile-time because the function reads it at compile time at "
                       "{}:{}{}",
                       name,
                       owner.path.string(),
                       owner.ast.location_of(*param.comptime_read),
                       param.comptime_read_in_const
                           ? ", in a `const` initializer (use `let` to read it at runtime)"
                           : "");
}

// Whether the parameter at `idx` takes its argument at compile time in this instantiation
[[nodiscard]] inline auto binds_comptime(const ast::AST&                       tree,
                                         const ast::function_expr&             fn,
                                         usize                                 idx,
                                         gsl::span<const gsl::not_null<type*>> arg_types) -> bool {
    return fn.parameters[idx].is_comptime || binds_implicit_comptime(tree, fn, idx, arg_types);
}

[[nodiscard]] inline auto any_param_generic(auto&& params) noexcept -> bool {
    return std::ranges::any_of(params, [](const auto& p) { return is_generic_type(*p); });
}

// Forwards an incomplete aggregate type to safely break cycles if possible
[[nodiscard]] inline auto forward_type(const mod::module_t&             target_mod,
                                       stdx::option<ast::type_modifier> mod,
                                       symbol& sym) noexcept -> stdx::option<type&> {
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node) { return stdx::none; }

    if (const auto decl{target_mod.ast.get_as_opt<ast::decl_stmt>(*node)}) {
        if (!decl->value) { return stdx::none; }

        // A recursive call only needs the signature, which is resolved before the body is
        if (decl->value->is<ast::function_expr>()) {
            const auto fn_ty{target_mod.get_sema_type_opt(*decl->value)};
            return (fn_ty && fn_ty->is_resolved()) ? fn_ty : stdx::none;
        }

        const auto is_agg{decl->value->is<ast::struct_expr>() ||
                          decl->value->is<ast::union_expr>() || decl->value->is<ast::enum_expr>()};
        if ((!mod || (!mod->is_ptr() && !mod->is_ref())) && !is_agg) { return stdx::none; }
        return target_mod.get_sema_type_opt(*decl->value);
    }
    return stdx::none;
}

// Whether an expression denotes a type (`^mut ^i32`, `&mut i32`, `[]u8`) rather than a
// value, so pointer and reference syntax in a type isn't taken for an rvalue capture
[[nodiscard]] inline auto
is_type_denoting_expr(const context&                          ctx,
                      const mod::module_t&                    mod,
                      ast::node_id                            id,
                      stdx::option<const symbol_table_stack&> tables = stdx::none) -> bool {
    // 1. Explicit sema type denoting a compile-time type
    if (const auto ty{mod.get_sema_type_opt(id)}) {
        // A not-yet-folded `[n]T` value shares the `type` kind but is not a type
        if (ty->get_kind() == type_kind::TYPE && !ty->get_data().is<types::deferred_array>()) {
            return true;
        }
    }
    // 2. Identifier representing a type symbol, primitive type, or keyword type
    if (const auto ident{mod.ast.get_as_opt<ast::identifier_expr>(id)}) {
        // A symbol still being resolved has no kind yet
        const auto is_type_sym{[](const symbol& sym) {
            return sym.has_kind() && sym.get_kind() == symbol_kind::TYPE;
        }};
        // A user binding spelled like a primitive (`@"i32"`) names that binding
        if (tables) {
            if (const auto sym{ctx.registry.lookup_with_depth(*tables, ident->name)}) {
                return is_type_sym(sym->symbol);
            }
        }
        if (syntax::token_type::is_int_type_lexeme(ident->name) ||
            syntax::token_type::is_primitive(id.get_token_type()) ||
            id.get_token_type() == syntax::token_type_t::TYPE_TYPE) {
            return true;
        }
        if (mod.root_table_idx) {
            if (const auto sym{ctx.registry.get_from_opt(*mod.root_table_idx, ident->name)}) {
                if (is_type_sym(*sym)) { return true; }
            }
        }
        if (ctx.prelude_index) {
            if (const auto b{ctx.registry.get_from_opt(*ctx.prelude_index, ident->name)}) {
                if (is_type_sym(*b)) { return true; }
            }
        }
    }
    // 3. Array type expression like `[]i32` or `[4]i32`, or a type spelled only as one (`dyn I`)
    if (const auto arr{mod.ast.get_as_opt<ast::array_expr>(id)}) {
        if (arr->is_type_expr) { return true; }
    }
    if (mod.ast.get_as_opt<ast::type_expr>(id)) { return true; }
    // 4. Recursive traversal for composite pointer/reference type expressions (e.g. `^mut ^i32`)
    if (const auto adr{mod.ast.get_as_opt<ast::address_of_expr>(id)}) {
        return is_type_denoting_expr(ctx, mod, adr->rhs, tables);
    }
    if (const auto ref{mod.ast.get_as_opt<ast::reference_expr>(id)}) {
        return is_type_denoting_expr(ctx, mod, ref->rhs, tables);
    }
    return false;
}

struct cabi_offenders {
    bool has_dyn{false};
    bool has_erased_fn{false};
    bool has_ref{false};
    bool has_nonabi_scalar{false};
};

// A leaf integer/float width with no defined C ABI representation. Off x86, `f80` is software
// and C has no 80-bit type to match.
[[nodiscard]] inline auto is_nonabi_scalar(const type& t, bool native_f80) noexcept -> bool {
    if (t.get_kind() == type_kind::INT) {
        const auto bits{int_width(t)};
        return bits != 8 && bits != 16 && bits != 32 && bits != 64;
    }
    if (t.get_kind() == type_kind::F80) { return !native_f80; }
    return t.get_kind() == type_kind::F16 || t.get_kind() == type_kind::F128;
}

[[nodiscard]] inline auto
scan_cabi_offenders(const type& t, bool native_f80, cabi_offenders acc = {}) -> cabi_offenders {
    if (is_nonabi_scalar(t, native_f80)) { acc.has_nonabi_scalar = true; }
    const auto scan{[native_f80](const type& inner, cabi_offenders so_far) {
        return scan_cabi_offenders(inner, native_f80, so_far);
    }};
    return t.get_data().visit(
        [&acc](types::dyn_t) {
            acc.has_dyn = true;
            return acc;
        },
        [&acc, &scan](types::reference r) {
            acc.has_ref = true;
            return scan(r.underlying, acc);
        },
        [acc, &scan](types::pointer p) { return scan(p.underlying, acc); },
        [acc, &scan](types::slice sl) { return scan(sl.underlying, acc); },
        [acc, &scan](types::array ar) { return scan(ar.underlying, acc); },
        [&acc, &scan](types::function fn) {
            if (fn.erased) { acc.has_erased_fn = true; }
            for (const auto param : fn.params) { acc = scan(*param, acc); }
            return scan(fn.return_type, acc);
        },
        [acc, &scan](types::deferred_array da) { return scan(da.underlying, acc); },
        [acc](const auto&) { return acc; });
}

// A vector in a C signature passes like `__m128` / `__m256` / `__m512`, which no other width has
[[nodiscard]] inline auto find_nonabi_vector(const type& t, u32 ptr_bits)
    -> stdx::option<const type&> {
    const auto scan{[ptr_bits](const type& inner) { return find_nonabi_vector(inner, ptr_bits); }};
    return t.get_data().visit(
        [&t, ptr_bits](types::vector vec) -> stdx::option<const type&> {
            const auto bits{vector_bits(vec, ptr_bits)};
            if (bits == 128 || bits == 256 || bits == 512) { return stdx::none; }
            return t;
        },
        [&scan](types::pointer p) { return scan(p.underlying); },
        [&scan](types::reference r) { return scan(r.underlying); },
        [&scan](types::slice sl) { return scan(sl.underlying); },
        [&scan](types::array ar) { return scan(ar.underlying); },
        [&scan](types::function fn) -> stdx::option<const type&> {
            for (const auto param : fn.params) {
                if (const auto found{scan(*param)}) { return found; }
            }
            return scan(fn.return_type);
        },
        [](const auto&) -> stdx::option<const type&> { return stdx::none; });
}

[[nodiscard]] inline auto nonabi_vector(const sema::context&   ctx,
                                        const type&            vec,
                                        const source_location& location) -> diagnostic {
    return diagnostic{fmt::format("'{}' has no C ABI; a vector crossing an `extern` boundary must "
                                  "be 16, 32, or 64 bytes",
                                  ctx.type_display_name(vec)),
                      error::ILLEGAL_EXTERN_VECTOR,
                      location};
}

// A function declared `@[testing]`, on its declaration or its literal
[[nodiscard]] inline auto declares_testing_fn(const mod::module_t& owner, ast::node_id node)
    -> bool {
    auto       decl{owner.ast.get_as_opt<ast::decl_stmt>(node)};
    const auto literal{[&]() -> stdx::option<const ast::function_expr&> {
        if (const auto fn{owner.ast.get_as_opt<ast::function_expr>(node)}) { return *fn; }
        if (decl && decl->value) { return owner.ast.get_as_opt<ast::function_expr>(*decl->value); }
        return stdx::none;
    }()};
    if (literal && literal->declaring_decl) {
        decl = owner.ast.get_as_opt<ast::decl_stmt>(*literal->declaring_decl);
    }
    const auto marked{[](const stdx::option<ast::attribute_list>& attributes) {
        return attributes && attributes->find(ast::attribute_kind::TESTING);
    }};
    return (decl && marked(decl->attributes)) || (literal && marked(literal->attributes));
}

// Whether a ctor's arguments include a stand-in `type`, making the instance a template
[[nodiscard]] inline auto is_abstract_instance(type_span args) -> bool {
    return std::ranges::any_of(args, [](gsl::not_null<type*> arg) {
        const auto& t{denoted_type(*arg)};
        return t.get_kind() == type_kind::TYPE && t.get_data().is<types::builtin_type>();
    });
}

} // namespace ghoti::sema::resolver_detail
