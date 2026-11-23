#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "stdx/assert.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

auto analyzer::info_struct(std::string_view            type_name,
                           gsl::span<const info_field> fields,
                           const source&               src) -> result<ip::index> {
    auto&                                type{*TRY(comp_.prelude_type(type_name, u_, src))};
    const auto&                          st{type.get_data().as<gs::types::struct_t>()};
    std::vector<stdx::option<ip::index>> elements(st.field_names.size());
    for (const auto& [name, value] : fields) {
        const auto at{std::ranges::find(st.field_names, name)};
        ASSERT(at != st.field_names.end(),
               "a prelude info struct declares every field it is given");
        elements[static_cast<usize>(at - st.field_names.begin())] = value;
    }

    return comp_.constants().intern(ip::aggregate_value{std::move(elements)}, type);
}

auto analyzer::info_union(std::string_view tag, stdx::option<ip::index> payload, const source& src)
    -> result<ip::index> {
    auto& pool{comp_.constants()};
    auto& info{*TRY(comp_.prelude_type("TypeInfo", u_, src))};
    return pool.intern(
        ip::union_value{
            .active_field = std::string{tag},
            .payload      = {payload.value_or(
                pool.intern(ip::void_value{}, builtin(gs::type_kind::VOID_)))},
        },
        info);
}

auto analyzer::info_variant(std::string_view enum_name, std::string_view variant, const source& src)
    -> result<ip::index> {
    auto&      type{*TRY(comp_.prelude_type(enum_name, u_, src))};
    const auto value{TRY(enum_constant(type, variant, src))};
    return *comptime_value(value);
}

auto analyzer::info_slice(std::string_view       elem_name,
                          std::vector<ip::index> elements,
                          const source&          src) -> result<ip::index> {
    auto& elem{*TRY(comp_.prelude_type(elem_name, u_, src))};
    auto& slice{comp_.ctx().get_slice(gs::types::mut::CONSTANT, false, elem)};
    std::vector<stdx::option<ip::index>> items{elements.begin(), elements.end()};
    return comp_.constants().intern(ip::aggregate_value{std::move(items)}, slice);
}

auto analyzer::info_name(std::string_view name) -> ip::index {
    auto& ctx{comp_.ctx()};
    auto& text{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    return comp_.constants().intern(std::string{name}, text);
}

auto analyzer::info_bool(bool value) -> ip::index {
    return comp_.constants().intern(value, builtin(gs::type_kind::BOOL));
}

auto analyzer::info_int(u64 value, gs::type& type) -> ip::index {
    return intern_int(comp_.constants(), big_int{static_cast<u128>(value)}, type);
}

auto analyzer::type_info(gs::type&                                    type,
                         stdx::option<const gs::resolved_attributes&> declared,
                         const source&                                src) -> result<ip::index> {
    auto&      ctx{comp_.ctx()};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    auto&      u16_type{ctx.get_int(16, false)};
    const auto int_info{[&](u64 bits, bool is_signed) -> result<ip::index> {
        const std::array fields{
            info_field{"bits", info_int(bits, u16_type)},
            info_field{"signedness",
                       TRY(info_variant("Signedness", is_signed ? "signed" : "unsigned", src))},
        };
        return info_union("int", TRY(info_struct("IntInfo", fields, src)), src);
    }};
    const auto pointer_info{[&](std::string_view tag, gs::type& child) -> result<ip::index> {
        const std::array fields{
            info_field{"child", interned_type(child)},
            info_field{"is_mut", info_bool(!type.is_constant())},
            info_field{"is_volatile", info_bool(type.is_volatile())},
        };
        return info_union(tag, TRY(info_struct("PointerInfo", fields, src)), src);
    }};

    switch (type.get_kind()) {
    case gs::type_kind::INT:   return int_info(gs::int_width(type), gs::is_signed_integer(type));
    case gs::type_kind::ISIZE:
    case gs::type_kind::USIZE: return int_info(comp_.ptr_bits(), gs::is_signed_integer(type));
    case gs::type_kind::COMPTIME_INT: return info_union("comptime_int", stdx::none, src);
    case gs::type_kind::F16:
    case gs::type_kind::F32:
    case gs::type_kind::F64:
    case gs::type_kind::F80:
    case gs::type_kind::F128:         {
        const std::array fields{
            info_field{"bits", info_int(gs::float_bits(type.get_kind()), u16_type)}};
        return info_union("float", TRY(info_struct("FloatInfo", fields, src)), src);
    }
    case gs::type_kind::COMPTIME_FLOAT: return info_union("comptime_float", stdx::none, src);
    case gs::type_kind::ENUM_LITERAL:   return info_union("enum_literal", stdx::none, src);
    case gs::type_kind::BOOL:           return info_union("bool", stdx::none, src);
    case gs::type_kind::VOID_:          return info_union("void", stdx::none, src);
    case gs::type_kind::NORETURN:       return info_union("noreturn", stdx::none, src);
    case gs::type_kind::OPAQUE:         return info_union("opaque", stdx::none, src);
    case gs::type_kind::TYPE:           return info_union("type", stdx::none, src);
    case gs::type_kind::POINTER:
        return pointer_info("pointer", type.get_data().as<gs::types::pointer>().underlying);
    case gs::type_kind::REFERENCE:
        return pointer_info("reference", type.get_data().as<gs::types::reference>().underlying);
    case gs::type_kind::SLICE: {
        const auto&      slice{type.get_data().as<gs::types::slice>()};
        const std::array fields{
            info_field{"child", interned_type(slice.underlying)},
            info_field{"sentinel", info_bool(slice.null_terminated)},
            info_field{"is_mut", info_bool(!type.is_constant())},
            info_field{"is_volatile", info_bool(type.is_volatile())},
        };
        return info_union("slice", TRY(info_struct("SliceInfo", fields, src)), src);
    }
    case gs::type_kind::ARRAY: {
        const auto&      arr{type.get_data().as<gs::types::array>()};
        const std::array fields{
            info_field{"child", interned_type(arr.underlying)},
            info_field{"len", info_int(arr.len, usize_type)},
            info_field{"sentinel", info_bool(arr.null_terminated)},
            info_field{"is_mut", info_bool(!type.is_constant())},
            info_field{"is_volatile", info_bool(type.is_volatile())},
        };
        return info_union("array", TRY(info_struct("ArrayInfo", fields, src)), src);
    }
    case gs::type_kind::VECTOR: {
        const auto&      vec{type.get_data().as<gs::types::vector>()};
        const std::array fields{
            info_field{"child", interned_type(vec.underlying)},
            info_field{"len", info_int(vec.len, usize_type)},
        };
        return info_union("vector", TRY(info_struct("VectorInfo", fields, src)), src);
    }
    case gs::type_kind::CLOSURE:
        return type_info(type.get_data().as<gs::types::closure_t>().signature, stdx::none, src);
    case gs::type_kind::FUNCTION: return function_info(type, declared, src);
    case gs::type_kind::ENUM:     return enum_info(type, src);
    case gs::type_kind::STRUCT:   return struct_info(type, src);
    case gs::type_kind::UNION:    return union_info(type, src);
    default:                      return info_union("internal", stdx::none, src);
    }
}

auto analyzer::function_info(gs::type&                                    type,
                             stdx::option<const gs::resolved_attributes&> declared,
                             const source& src) -> result<ip::index> {
    const auto&            fn{type.get_data().as<gs::types::function>()};
    std::vector<ip::index> params;
    for (const auto param : fn.params) { params.emplace_back(interned_type(*param)); }
    auto& type_type{builtin(gs::type_kind::TYPE)};
    auto& params_type{comp_.ctx().get_slice(gs::types::mut::CONSTANT, false, type_type)};
    std::vector<stdx::option<ip::index>> items{params.begin(), params.end()};

    const auto       attrs{declared ? *declared : gs::resolved_attributes{}};
    const auto       inlining{attrs.inlining.value_or(ast::inline_mode::DEFAULT)};
    const auto       visibility{attrs.visibility.value_or(ast::symbol_visibility::DEFAULT)};
    const std::array fields{
        info_field{"params",
                   comp_.constants().intern(ip::aggregate_value{std::move(items)}, params_type)},
        info_field{"return_type", interned_type(fn.return_type)},
        info_field{"variadic", info_bool(fn.is_variadic)},
        info_field{"has_self", info_bool(fn.has_self)},
        info_field{"callconv",
                   TRY(info_variant("CallConv", ast::calling_convention_name(fn.conv), src))},
        info_field{"erased", info_bool(fn.erased)},
        info_field{"inline_mode",
                   TRY(info_variant("Inline", ast::inline_mode_name(inlining), src))},
        info_field{"naked", info_bool(attrs.naked)},
        info_field{"no_builtins", info_bool(attrs.no_builtins)},
        info_field{"discardable", info_bool(attrs.discardable)},
        info_field{"cold", info_bool(attrs.cold)},
        info_field{"alignment",
                   info_int(attrs.alignment.value_or(0), builtin(gs::type_kind::USIZE))},
        info_field{"visibility",
                   TRY(info_variant("Visibility", ast::symbol_visibility_name(visibility), src))},
    };
    return info_union("function", TRY(info_struct("FnInfo", fields, src)), src);
}

auto analyzer::enum_info(gs::type& type, const source& src) -> result<ip::index> {
    TRY(comp_.require_fields(u_, src, type));
    const auto& en{type.get_data().as<gs::types::enum_t>()};
    const auto  n{comp_.nominal_of(type)};
    if (!n) { return comp_.unsupported(u_, src, "the `@typeInfo` of an enum UIR didn't declare"); }

    std::vector<ip::index> fields;
    for (usize i{0}; i < en.variant_names.size(); ++i) {
        const std::array field{
            info_field{"name", info_name(en.variant_names[i])},
            info_field{"value",
                       intern_int(comp_.constants(),
                                  big_int{static_cast<i128>(n->enum_values[i])},
                                  builtin(gs::type_kind::COMPTIME_INT))},
        };
        fields.emplace_back(TRY(info_struct("EnumFieldInfo", field, src)));
    }

    const std::array info{
        info_field{"tag_type", interned_type(en.underlying)},
        info_field{"fields", TRY(info_slice("EnumFieldInfo", std::move(fields), src))},
        info_field{"exhaustive", info_bool(!en.non_exhaustive)},
    };
    return info_union("enum", TRY(info_struct("EnumInfo", info, src)), src);
}

auto analyzer::struct_info(gs::type& type, const source& src) -> result<ip::index> {
    TRY(comp_.require_fields(u_, src, type));
    const auto& st{type.get_data().as<gs::types::struct_t>()};
    auto&       ctx{comp_.ctx()};
    auto& opaque_ptr{ctx.get_pointer(gs::types::mut::CONSTANT, builtin(gs::type_kind::OPAQUE))};
    auto& pool{comp_.constants()};

    std::vector<ip::index> fields;
    for (usize i{0}; i < st.field_names.size(); ++i) {
        const auto       default_value{TRY(field_default_value(type, i, src))};
        const std::array field{
            info_field{"name", info_name(st.field_names[i])},
            info_field{"type", interned_type(st.type_at(i))},
            info_field{"default_value",
                       default_value ? pool.intern(ip::address_value{.symbol  = {},
                                                                     .pointee = {*default_value}},
                                                   opaque_ptr)
                                     : pool.intern(ip::nullptr_value{}, opaque_ptr)},
            info_field{"alignment",
                       info_int(st.explicit_field_alignment(i), builtin(gs::type_kind::USIZE))},
            info_field{"is_pub", info_bool(st.is_field_public(i))},
        };
        fields.emplace_back(TRY(info_struct("StructFieldInfo", field, src)));
    }

    const auto       backing{gs::packed_backing_bits(st, comp_.ptr_bits()).value_or(0)};
    const std::array info{
        info_field{"fields", TRY(info_slice("StructFieldInfo", std::move(fields), src))},
        info_field{"is_extern", info_bool(st.is_c_abi)},
        info_field{"is_packed", info_bool(st.is_packed)},
        info_field{"backing_bits", info_int(backing, ctx.get_int(32, false))},
    };
    return info_union("struct", TRY(info_struct("StructInfo", info, src)), src);
}

auto analyzer::union_info(gs::type& type, const source& src) -> result<ip::index> {
    TRY(comp_.require_fields(u_, src, type));
    const auto& ut{type.get_data().as<gs::types::union_t>()};

    std::vector<ip::index> fields;
    for (usize i{0}; i < ut.field_names.size(); ++i) {
        const std::array field{
            info_field{"name", info_name(ut.field_names[i])},
            info_field{"type", interned_type(ut.type_at(i))},
        };
        fields.emplace_back(TRY(info_struct("UnionFieldInfo", field, src)));
    }

    const std::array info{
        info_field{"fields", TRY(info_slice("UnionFieldInfo", std::move(fields), src))},
        info_field{"is_extern", info_bool(ut.is_c_abi)},
        info_field{"is_packed", info_bool(ut.is_packed)},
        info_field{"tagged", info_bool(!ut.is_untagged)},
        info_field{"tag_type", interned_type(ut.tag_enum.value_or(builtin(gs::type_kind::VOID_)))},
    };
    return info_union("union", TRY(info_struct("UnionInfo", info, src)), src);
}

auto analyzer::field_default_value(gs::type& type, usize index, const source& src)
    -> result<stdx::option<ip::index>> {
    // A default that can't be folded reports none
    const auto defaults{TRY(field_defaults(type, src))};
    if (index >= defaults.size() || !defaults[index].present()) { return stdx::none; }
    const auto scope{comptime_scope()};
    const auto value{field_default(type, index, defaults[index], src)};
    if (!value) {
        if (value.error() == stop::UNSUPPORTED) { return stdx::err{stop::UNSUPPORTED}; }
        return stdx::none;
    }

    return comptime_value(*value);
}

auto analyzer::builtin_type_info(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@typeInfo` of this arity"); }
    const auto value{TRY(resolve_value(args.front(), src))};

    // A function passed by name reflects its declaration's attributes too
    if (const auto fn{value.as_opt<function_ref>()}) {
        if (!fn->fn->attributes) {
            return comp_.unsupported(u_, src, "the `@typeInfo` of a function with attributes");
        }

        return operand{TRY(type_info(*fn->fn->type, *fn->fn->attributes, src))};
    }

    if (value.is<generic_ref>()) {
        return comp_.unsupported(u_, src, "the `@typeInfo` of a generic function");
    }

    auto& type{*TRY(denoted_type(value, src))};
    return operand{TRY(type_info(type, stdx::none, src))};
}

} // namespace ghoti::uir::sema
