#include "compiler/sema/context.hh"

#include <concepts>
#include <utility>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"

namespace ghoti::sema {

auto context::get_poison() -> type& {
    auto& poison{*pool[{type_kind::POISON, types::mut::CONSTANT}]};
    poison.resolve_if<types::poison>();
    return poison;
}

auto context::get_pointer(types::mut::mutability_modifiers mutability, type& underlying) -> type& {
    auto& type{*pool[{type_kind::POINTER, mutability, underlying}]};
    type.resolve_if<types::pointer>(underlying);
    return type;
}

auto context::get_reference(types::mut::mutability_modifiers mutability, type& underlying)
    -> type& {
    auto& type{*pool[{type_kind::REFERENCE, mutability, underlying}]};
    type.resolve_if<types::reference>(underlying);
    return type;
}

auto context::get_array(types::mut::mutability_modifiers mutability,
                        bool                             null_terminated,
                        usize                            size,
                        type&                            underlying) -> type& {
    auto& type{*pool[{type_kind::ARRAY, mutability, null_terminated, size, underlying}]};
    type.resolve_if<types::array>(underlying, size, null_terminated);
    return type;
}

auto context::get_slice(types::mut::mutability_modifiers mutability,
                        bool                             null_terminated,
                        type&                            underlying) -> type& {
    auto& type{*pool[{type_kind::SLICE, mutability, null_terminated, underlying}]};
    type.resolve_if<types::slice>(underlying, null_terminated);
    return type;
}

namespace {

auto inject_types(symbol_table& prelude, type_pool& pool) -> void {
    PROFILE_FUNCTION();
    const auto inject_type = [&](const syntax::keyword_t& keyword, type_kind kind) -> void {
        auto& type{*pool[{kind, types::mut::CONSTANT}]};
        ASSERT(!type.is_resolved(), "Builtin types should only be resolved once");
        type.resolve<types::builtin_type>();

        prelude.insert_unchecked(keyword.name, symbols::builtin{keyword, type});
        auto& symbol{prelude.get(keyword.name)};
        symbol.set_kind(symbol_kind::TYPE);
        symbol.set_status(symbol_status::RESOLVED);
    };

    namespace kws = syntax::keywords;

    // Primitives
    inject_type(kws::I32, type_kind::I32);
    inject_type(kws::I64, type_kind::I64);
    inject_type(kws::ISIZE, type_kind::ISIZE);
    inject_type(kws::U32, type_kind::U32);
    inject_type(kws::U64, type_kind::U64);
    inject_type(kws::USIZE, type_kind::USIZE);
    inject_type(kws::F32, type_kind::F32);
    inject_type(kws::F64, type_kind::F64);
    inject_type(kws::U8, type_kind::U8);
    inject_type(kws::BOOL, type_kind::BOOL);
    inject_type(kws::VOID, type_kind::VOID_);

    // Special types
    inject_type(kws::TYPE, type_kind::TYPE);
    inject_type(kws::AUTO, type_kind::AUTO);
    inject_type(kws::OPAQUE, type_kind::OPAQUE);
    inject_type(kws::UNDEFINED, type_kind::UNDEFINED);
    inject_type(kws::NULLPTR, type_kind::NULLPTR);
    inject_type(kws::NORETURN, type_kind::NORETURN);
}

auto inject_functions(symbol_table& prelude, type_pool& pool) -> void {
    PROFILE_FUNCTION();
    const auto inject_function = [&](const syntax::builtin_t& builtin,
                                     types::builtin_params&&  param_types,
                                     type&                    return_type) -> void {
        for (const auto& param_type : param_types) {
            ASSERT(param_type->is_resolved(), "Builtins must be fully resolved");
        }
        ASSERT(return_type.is_resolved(), "Builtins must be fully resolved");

        types::key_t key{type_kind::FUNCTION, types::mut::CONSTANT};
        key.imprint(builtin);
        auto& type{*pool[key]};
        ASSERT(!type.is_resolved(), "Builtin functions should only be resolved once");
        type.resolve<types::builtin_function>(std::move(param_types), return_type);

        prelude.insert_unchecked(builtin.name, symbols::builtin{builtin, type});
        auto& symbol{prelude.get(builtin.name)};
        symbol.set_kind(symbol_kind::CALLABLE);
        symbol.set_status(symbol_status::RESOLVED);
    };

    namespace bis     = syntax::builtins;
    const auto params = [&](std::same_as<type&> auto&&... params) -> auto {
        return types::builtin_params{&params...};
    };

    // Common types
    auto& t_void{*pool[{type_kind::VOID_, types::mut::CONSTANT}]};
    auto& t_type{*pool[{type_kind::TYPE, types::mut::CONSTANT}]};
    auto& t_usize{*pool[{type_kind::USIZE, types::mut::CONSTANT}]};
    auto& t_auto{*pool[{type_kind::AUTO, types::mut::CONSTANT}]};
    auto& t_noreturn{*pool[{type_kind::NORETURN, types::mut::CONSTANT}]};

    // C-string
    auto& t_u8{*pool[{type_kind::U8, types::mut::CONSTANT}]};
    auto& t_c_str{*pool[{type_kind::SLICE, types::mut::CONSTANT, true, t_u8}]};
    t_c_str.resolve_if<types::slice>(*pool[{type_kind::U8, types::mut::CONSTANT}], true);

    inject_function(bis::ALIGN_CAST, params(t_type, t_auto), t_auto);
    inject_function(bis::PTR_CAST, params(t_type, t_auto), t_auto);
    inject_function(bis::BIT_CAST, params(t_type, t_auto), t_auto);
    inject_function(bis::CONST_CAST, params(t_auto), t_auto);
    inject_function(bis::VOLATILE_CAST, params(t_auto), t_auto);
    inject_function(bis::AS, params(t_type, t_auto), t_auto);

    inject_function(bis::INT_FROM_PTR, params(t_auto), t_usize);
    inject_function(bis::PTR_FROM_INT, params(t_type, t_usize), t_auto);
    inject_function(bis::PTR_FROM_ARRAY, params(t_auto), t_auto);
    inject_function(bis::SLICE_FROM_PTR, params(t_auto, t_usize), t_auto);

    inject_function(bis::ALIGN_OF, params(t_auto), t_usize);
    inject_function(bis::SIZE_OF, params(t_auto), t_usize);
    inject_function(bis::TYPE_OF, params(t_auto), t_type);
    inject_function(bis::THIS, params(), t_type);
    inject_function(bis::TAG_NAME, params(t_auto), t_c_str);

    inject_function(bis::MEMCPY, params(t_auto, t_auto), t_void);
    inject_function(bis::MEMSET, params(t_auto, t_auto), t_void);
    inject_function(bis::MEMMOVE, params(t_auto, t_auto), t_void);

    inject_function(bis::MUL_ADD, params(t_type, t_auto, t_auto, t_auto), t_auto);
    inject_function(bis::CLZ, params(t_auto), t_usize);
    inject_function(bis::CTZ, params(t_auto), t_usize);
    inject_function(bis::POP_COUNT, params(t_auto), t_usize);
    inject_function(bis::SQRT, params(t_auto), t_auto);
    inject_function(bis::SIN, params(t_auto), t_auto);
    inject_function(bis::COS, params(t_auto), t_auto);
    inject_function(bis::TAN, params(t_auto), t_auto);
    inject_function(bis::EXP, params(t_auto), t_auto);
    inject_function(bis::EXP2, params(t_auto), t_auto);
    inject_function(bis::LOG, params(t_auto), t_auto);
    inject_function(bis::LOG2, params(t_auto), t_auto);
    inject_function(bis::LOG10, params(t_auto), t_auto);
    inject_function(bis::ABS, params(t_auto), t_auto);
    inject_function(bis::FLOOR, params(t_auto), t_auto);
    inject_function(bis::CEIL, params(t_auto), t_auto);

    inject_function(bis::C_VA_START, params(t_auto), t_void);
    inject_function(bis::C_VA_ARG, params(t_auto, t_type), t_auto);
    inject_function(bis::C_VA_COPY, params(t_auto, t_auto), t_void);
    inject_function(bis::C_VA_END, params(t_auto), t_void);

    inject_function(bis::TARGET_OS, params(), t_c_str);
    inject_function(bis::TARGET_ARCH, params(), t_c_str);
    inject_function(bis::TARGET_TRIPLE, params(), t_c_str);

    inject_function(bis::SET_EVAL_RECURSION_LIMIT, params(t_usize), t_void);
    inject_function(bis::SET_MAIN_SYMBOL, params(t_c_str), t_void);

    inject_function(bis::PANIC, params(t_c_str), t_noreturn);
}

} // namespace

auto context::inject_prelude() -> void {
    PROFILE_FUNCTION();
    if (prelude_index) { return; }
    prelude_index.emplace(registry.create());

    auto& prelude{registry.get(*prelude_index)};
    inject_types(prelude, pool);
    inject_functions(prelude, pool);
}

auto context::get_builtin_resolved_type(type_kind kind) -> type& {
    auto& type{*pool[{kind, types::mut::CONSTANT}]};
    ASSERT(type.is_resolved(), "Builtin type was not already resolved");
    return type;
}

} // namespace ghoti::sema
