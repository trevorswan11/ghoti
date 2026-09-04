#include "compiler/sema/context.hh"

#include <concepts>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/passes/symbol_collector.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/keywords.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::sema {

auto context::get_poison() -> type& {
    auto& poison{*pool[{type_kind::POISON, types::mut::CONSTANT}]};
    poison.resolve_if<types::poison>();
    return poison;
}

auto context::get_int(u16 bits, bool is_signed, types::mut::mutability_modifiers mutability)
    -> type& {
    auto& type{*pool[{type_kind::INT, mutability, bits, is_signed}]};
    type.resolve_if<types::integer>(bits, is_signed);
    return type;
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
    const auto register_symbol = [&](const syntax::keyword_t& keyword, type& type) -> void {
        prelude.insert_unchecked(keyword.name, symbols::builtin{keyword, type});
        auto& symbol{prelude.get(keyword.name)};
        symbol.set_kind(symbol_kind::TYPE);
        symbol.set_status(symbol_status::RESOLVED);
    };

    const auto inject_type = [&](const syntax::keyword_t& keyword, type_kind kind) -> void {
        auto& type{*pool[{kind, types::mut::CONSTANT}]};
        ASSERT(!type.is_resolved(), "Builtin types should only be resolved once");
        type.resolve<types::builtin_type>();
        register_symbol(keyword, type);
    };

    namespace kws = syntax::keywords;

    inject_type(kws::ISIZE, type_kind::ISIZE);
    inject_type(kws::USIZE, type_kind::USIZE);
    inject_type(kws::F16, type_kind::F16);
    inject_type(kws::F32, type_kind::F32);
    inject_type(kws::F64, type_kind::F64);
    inject_type(kws::F80, type_kind::F80);
    inject_type(kws::F128, type_kind::F128);
    inject_type(kws::CONSTEXPR_INT, type_kind::CONSTEXPR_INT);
    inject_type(kws::CONSTEXPR_FLOAT, type_kind::CONSTEXPR_FLOAT);
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
    auto& t_bool{*pool[{type_kind::BOOL, types::mut::CONSTANT}]};

    // C-string
    auto& t_u8{*pool[{type_kind::INT, types::mut::CONSTANT, u16{8}, false}]};
    auto& t_c_str{*pool[{type_kind::SLICE, types::mut::CONSTANT, true, t_u8}]};
    t_c_str.resolve_if<types::slice>(t_u8, true);

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
    inject_function(bis::FIELD_PARENT_PTR, params(t_type, t_c_str, t_auto), t_auto);

    inject_function(bis::ALIGN_OF, params(t_auto), t_usize);
    inject_function(bis::SIZE_OF, params(t_auto), t_usize);
    inject_function(bis::BIT_SIZE_OF, params(t_auto), t_usize);
    inject_function(bis::TYPE_OF, params(t_auto), t_type);
    inject_function(bis::THIS, params(), t_type);
    inject_function(bis::TAG_NAME, params(t_auto), t_c_str);
    inject_function(bis::TYPE_NAME, params(t_auto), t_c_str);

    inject_function(bis::MEMCPY, params(t_auto, t_auto), t_void);
    inject_function(bis::MEMSET, params(t_auto, t_auto), t_void);
    inject_function(bis::MEMMOVE, params(t_auto, t_auto), t_void);

    inject_function(bis::MUL_ADD, params(t_type, t_auto, t_auto, t_auto), t_auto);
    inject_function(bis::CLZ, params(t_auto), t_usize);
    inject_function(bis::CTZ, params(t_auto), t_usize);
    inject_function(bis::POP_COUNT, params(t_auto), t_usize);
    inject_function(bis::ABS, params(t_auto), t_auto);

    inject_function(bis::MIN, params(t_auto, t_auto), t_auto);
    inject_function(bis::MAX, params(t_auto, t_auto), t_auto);
    inject_function(bis::DIV_TRUNC, params(t_auto, t_auto), t_auto);
    inject_function(bis::DIV_FLOOR, params(t_auto, t_auto), t_auto);
    inject_function(bis::REM, params(t_auto, t_auto), t_auto);
    inject_function(bis::MOD, params(t_auto, t_auto), t_auto);
    inject_function(bis::ADD_WITH_OVERFLOW, params(t_auto, t_auto, t_auto), t_bool);
    inject_function(bis::SUB_WITH_OVERFLOW, params(t_auto, t_auto, t_auto), t_bool);
    inject_function(bis::MUL_WITH_OVERFLOW, params(t_auto, t_auto, t_auto), t_bool);
    inject_function(bis::SHL_WITH_OVERFLOW, params(t_auto, t_auto, t_auto), t_bool);

    inject_function(bis::C_VA_START, params(t_auto), t_void);
    inject_function(bis::C_VA_ARG, params(t_auto, t_type), t_auto);
    inject_function(bis::C_VA_COPY, params(t_auto, t_auto), t_void);
    inject_function(bis::C_VA_END, params(t_auto), t_void);

    inject_function(bis::TARGET_OS, params(), t_c_str);
    inject_function(bis::TARGET_ARCH, params(), t_c_str);
    inject_function(bis::TARGET_TRIPLE, params(), t_c_str);
    inject_function(bis::TARGET_ABI, params(), t_c_str);
    inject_function(bis::TARGET_PTR_BITS, params(), t_usize);
    inject_function(bis::TARGET_ENDIAN, params(), t_c_str);
    inject_function(bis::TARGET_FAMILY, params(), t_c_str);

    inject_function(bis::SET_EVAL_RECURSION_LIMIT, params(t_usize), t_void);
    inject_function(bis::SET_MAIN_SYMBOL, params(t_c_str), t_void);

    inject_function(bis::PANIC, params(t_c_str), t_noreturn);
    inject_function(bis::TRAP, params(), t_noreturn);
    inject_function(bis::COMPILE_ERROR, params(t_c_str), t_noreturn);

    // `@implements(T | value, I)` -> bool (constexpr)
    inject_function(bis::IMPLEMENTS, params(t_auto, t_auto), t_bool);
    inject_function(bis::FN_CTX, params(), t_auto);

    inject_function(bis::SRC, params(), t_auto);
    inject_function(bis::EXPECT, params(t_bool), t_bool);
    inject_function(bis::REQUIRE, params(t_bool), t_void);
    inject_function(bis::SKIP, params(t_c_str), t_noreturn);

    inject_function(bis::ASSERT, params(t_bool, t_c_str), t_void);
    inject_function(bis::VERIFY, params(t_bool, t_c_str), t_void);
    inject_function(bis::DYN_CAST, params(t_type, t_auto), t_auto);

    // Prelude enums aren't resolved until `inject_builtin_module` runs so use auto here
    inject_function(bis::ATOMIC_LOAD, params(t_type, t_auto, t_auto), t_auto);
    inject_function(bis::ATOMIC_STORE, params(t_auto, t_auto, t_auto), t_void);
    inject_function(bis::ATOMIC_RMW, params(t_type, t_auto, t_auto, t_auto, t_auto), t_auto);
    inject_function(
        bis::CMPXCHG_WEAK, params(t_type, t_auto, t_auto, t_auto, t_auto, t_auto, t_auto), t_bool);
    inject_function(bis::CMPXCHG_STRONG,
                    params(t_type, t_auto, t_auto, t_auto, t_auto, t_auto, t_auto),
                    t_bool);
    inject_function(bis::FENCE, params(t_auto), t_void);
}

constexpr std::string_view BUILTIN_MODULE_SOURCE{
#include "builtin.gh.inc"
};

constexpr std::string_view BUILTIN_NAMESPACE{"builtin"};

auto inject_builtin_module(context& ctx, usize prelude_idx) -> void {
    PROFILE_FUNCTION();

    auto& enum_mod{ctx.modules.get_or_create_builtin_module(BUILTIN_MODULE_SOURCE)};
    symbol_collector::collect_symbols(enum_mod, ctx);
    type_resolver::resolve_types(enum_mod, ctx);
    VERIFY(!enum_mod.is_poisoned() && ctx.diags.empty(),
           "the compiler-provided `builtin` module must resolve cleanly");

    // Expose the module under one prelude name, reusing the ordinary module-access machinery
    auto& mod_type{*ctx.pool[{type_kind::MODULE, types::mut::CONSTANT, *enum_mod.root_table_idx}]};
    mod_type.resolve_if<types::module>(enum_mod);

    auto& prelude{ctx.registry.get(prelude_idx)};
    prelude.insert_unchecked(
        BUILTIN_NAMESPACE,
        symbols::builtin{syntax::typed_identifier{BUILTIN_NAMESPACE, syntax::token_type_t::IDENT},
                         mod_type});
    auto& injected{prelude.get(BUILTIN_NAMESPACE)};
    injected.set_kind(symbol_kind::MODULE);
    injected.set_status(symbol_status::RESOLVED);
}

} // namespace

auto context::inject_prelude() -> void {
    PROFILE_FUNCTION();
    if (prelude_index) { return; }
    prelude_index.emplace(registry.create());

    inject_types(registry.get(*prelude_index), pool);
    inject_functions(registry.get(*prelude_index), pool);
    inject_builtin_module(*this, *prelude_index);
}

auto context::get_builtin_resolved_type(type_kind kind) -> type& {
    auto& type{*pool[{kind, types::mut::CONSTANT}]};
    ASSERT(type.is_resolved(), "Builtin type was not already resolved");
    return type;
}

auto context::lookup_constexpr_binding(std::string_view name) const
    -> stdx::option<const gir::const_value&> {
    for (const auto& frame : constexpr_binding_frames | std::views::reverse) {
        if (const auto it{frame.find(name)}; it != frame.end()) { return it->second; }
    }
    return stdx::none;
}

auto context::get_builtin_type(std::string_view name) -> type& {
    VERIFY(prelude_index, "get_builtin_type requires inject_prelude to have run");
    auto& enum_mod{modules.builtin_module()};
    auto& enum_sym{registry.get(*enum_mod.root_table_idx).get(name)};
    return enum_mod.get_sema_type(enum_sym.get_data().as<symbols::node_t>());
}

auto context::type_display_name(const type& t) const -> std::string {
    const type* denoted{&t};
    if (denoted->get_kind() == type_kind::TYPE) {
        if (const auto meta{denoted->get_data().as_opt<types::meta_type>()}) {
            denoted = &meta->instance;
        }
    }

    if (const auto it{user_type_names.find(denoted)}; it != user_type_names.end()) {
        return std::string{it->second};
    }
    return denoted->to_string();
}

} // namespace ghoti::sema
