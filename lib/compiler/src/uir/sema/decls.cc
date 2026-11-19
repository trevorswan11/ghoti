#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// The declaration flags this phase gives a meaning to
constexpr auto HANDLED_FLAGS{decl_flags::PUBLIC | decl_flags::CONSTANT | decl_flags::LET |
                             decl_flags::MUT | decl_flags::COMPTIME};

constexpr std::string_view GLOBAL_CONSTANT{
    "global variable initializer must be a constant expression"};

// A function whose every call runs at compile time, which has no runtime body
[[nodiscard]] auto is_comptime_only(const gs::type& return_type) noexcept -> bool {
    return return_type.get_kind() == gs::type_kind::TYPE;
}

} // namespace

auto compilation::analyze_decl(const unit& u, decl_id decl, const source& use) -> result<operand> {
    const auto& d{u.uir->decl_at(decl)};
    switch (decls_[u.index][std::to_underlying(decl)].status) {
    case decl_status::RESOLVED: return *decls_[u.index][std::to_underlying(decl)].value;
    case decl_status::FAILED:   return stdx::err{stop::POISON};
    case decl_status::IN_PROGRESS:
        return report(u,
                      use,
                      gs::error::CYCLIC_DEPENDENCY,
                      fmt::format("'{}' is used during its own resolution", u.uir->string(d.name)));
    case decl_status::UNRESOLVED: break;
    }
    if (is_deferred_error(u, d)) {
        decls_[u.index][std::to_underlying(decl)].status = decl_status::FAILED;
        return report(u, use, gs::error::COMPILE_ERROR_REACHED, deferred_error_message(u, d));
    }

    decls_[u.index][std::to_underlying(decl)].status = decl_status::IN_PROGRESS;
    analyzer   a{*this, u, decl};
    const auto value{a.analyze_decl(decl)};
    auto&      state{decls_[u.index][std::to_underlying(decl)]};
    if (!value) {
        state.status = decl_status::FAILED;
        return stdx::err{value.error()};
    }
    state.status = decl_status::RESOLVED;
    state.value.emplace(*value);
    return *value;
}

auto compilation::own_symbol(const unit& u, decl_id decl, std::string symbol)
    -> const std::string& {
    auto& owned{decls_[u.index][std::to_underlying(decl)].global_symbol};
    owned.emplace(stdx::make_box<std::string>(std::move(symbol)));
    return **owned;
}

auto compilation::symbol_for(const unit& u, const decl& d) const -> std::string {
    // Qualifying names that collide across modules arrives with multi-module programs
    return std::string{u.uir->string(d.name)};
}

auto compilation::linkage_of(const decl& d) noexcept -> gir::linkage {
    if (d.has(decl_flags::EXPORT)) { return gir::linkage::EXPORT; }
    if (d.has(decl_flags::PUBLIC)) { return gir::linkage::PUBLIC; }
    if (d.has(decl_flags::EXTERN)) { return gir::linkage::EXTERN; }
    return gir::linkage::INTERNAL;
}

auto compilation::declare_function(const unit& u, decl_id decl, inst_id func) -> result<operand> {
    PROFILE_FUNCTION();
    const auto&   m{*u.uir};
    const auto&   d{m.decl_at(decl)};
    const auto&   src{m.src_of(func)};
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{rec.body_field()};
    const auto    ret{rec.body_field()};
    DISCARD(rec.body_field());
    const auto conv{static_cast<ast::calling_convention>(rec.u32_field())};
    const auto fn_flags{rec.u32_field()};
    const auto attrs{rec.body_field()};
    const auto bound_count{rec.count()};

    if ((fn_flags & flags::BODYLESS) != 0) {
        return unsupported(u, src, "a function without a body");
    }
    if ((fn_flags & ~flags::EXPLICIT_CONV) != 0) {
        return unsupported(u, src, "a variadic, extern, erased or `impl` function");
    }
    if (!attrs.empty()) { return unsupported(u, src, "function attributes"); }
    if (bound_count != 0) { return unsupported(u, src, "`impl` parameter bounds"); }

    analyzer                              sig{*this, u};
    std::vector<gsl::not_null<gs::type*>> param_types;
    for (const auto param : params) {
        record_reader p{m, m.data_of(param).pl.extra};
        DISCARD(p.str_field());
        const auto  type_body{p.body_field()};
        const auto  default_body{p.body_field()};
        const auto  param_flags{p.u32_field()};
        const auto  modifier{p.u32_field()};
        const auto& param_src{m.src_of(param)};
        if (!default_body.empty()) { return unsupported(u, param_src, "a default argument"); }
        if ((param_flags & (flags::PARAM_COMPTIME | flags::PARAM_PACK | flags::PARAM_SELF |
                            flags::PARAM_WRITTEN)) != 0) {
            return unsupported(u, param_src, "a comptime, pack, self or written parameter");
        }
        if (modifier != 0) { return unsupported(u, param_src, "a parameter modifier"); }
        auto& type{*TRY(sig.eval_type(type_body, param_src))};
        if (!gs::is_value_type(type.get_kind()) || gs::is_comptime_numeric(type.get_kind())) {
            return unsupported(u, param_src, "a parameter of a comptime-only or generic type");
        }
        param_types.emplace_back(&type);
    }
    auto&      return_type{*TRY(sig.eval_type(ret, src))};
    const bool comptime_only{is_comptime_only(return_type)};
    if (return_type.get_kind() != gs::type_kind::VOID_ && !comptime_only &&
        (!gs::is_value_type(return_type.get_kind()) ||
         gs::is_comptime_numeric(return_type.get_kind()))) {
        return unsupported(u, src, "a return type that is comptime-only or generic");
    }

    auto span{ctx_.pool.get_many_unsafe(param_types.size())};
    for (usize i{0}; i < param_types.size(); ++i) { span[i] = param_types[i]; }
    auto& fn_type{ctx_.get_function(span, return_type, false, conv, false)};

    auto& fn{*instances_.emplace_back(stdx::make_box<instance>(instance{
        .owner         = &units_[u.index],
        .decl          = decl,
        .func          = func,
        .symbol        = symbol_for(u, d),
        .type          = &fn_type,
        .params        = std::move(param_types),
        .return_type   = &return_type,
        .linkage       = linkage_of(d),
        .queued        = false,
        .comptime_only = comptime_only,
    }))};
    request_body(fn);
    return function_ref{&fn};
}

auto analyzer::analyze_decl(decl_id decl) -> result<operand> {
    PROFILE_FUNCTION();
    const auto& d{mod().decl_at(decl)};
    ASSERT(d.node, "a module-level declaration has a source node");
    const source src{*d.node};
    if (d.owner) { return comp_.unsupported(u_, src, "a member declaration"); }
    const bool comptime_mut{d.has(decl_flags::COMPTIME) && d.has(decl_flags::MUT)};
    if ((d.flags & ~HANDLED_FLAGS) != decl_flags::NONE || comptime_mut ||
        d.extern_target.len != 0 || d.link_name.len != 0) {
        return comp_.unsupported(u_, src, "an extern, export, weak or comptime declaration");
    }
    if (!mod().body(d.attrs_body).empty()) {
        return comp_.unsupported(u_, src, "declaration attributes");
    }
    if (!d.value_body) { return comp_.unsupported(u_, src, "a declaration without a value"); }

    stdx::option<gs::type&> declared;
    if (d.type_body) { declared.emplace(*TRY(eval_type(mod().body(d.type_body), src))); }
    const auto value{eval_comptime(mod().body(d.value_body))};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.report_runtime_need(
            u_, src, gs::error::COMPTIME_EVALUATION_FAILED, std::string{GLOBAL_CONSTANT});
    }
    if (!value) { return stdx::err{value.error()}; }
    if (value->is<function_ref>()) {
        if (declared) {
            return comp_.unsupported(u_, src, "a function declaration with an explicit type");
        }
        return *value;
    }
    return analyze_global(decl, *value, declared, src);
}

auto analyzer::analyze_global(decl_id                 decl,
                              const operand&          value,
                              stdx::option<gs::type&> declared,
                              const source&           src) -> result<operand> {
    const auto& d{mod().decl_at(decl)};
    const auto  constant_index{comptime_value(value)};
    if (!constant_index) { return comp_.unsupported(u_, src, "a global of this kind of value"); }

    auto& pool{comp_.constants()};
    // An untyped type alias is listed by name, with no storage of its own
    if (pool.is_type(*constant_index)) {
        if (d.has(decl_flags::LET) || d.has(decl_flags::MUT)) {
            return comp_.report(u_,
                                src,
                                gs::error::MUTABLE_TYPE_BINDING,
                                std::string{messages::MUTABLE_TYPE_BINDING});
        }
        if (declared && declared->get_kind() != gs::type_kind::TYPE) {
            return comp_.unsupported(u_, src, "a type stored in a typed global");
        }
        if (!declared) {
            comp_.gir().add_type(comp_.symbol_for(u_, d), pool.type_at(*constant_index));
        }
        return value;
    }

    // An untyped `undefined` has nothing to store
    if (!declared && type_of(value).get_kind() == gs::type_kind::UNDEFINED) { return value; }
    const auto stored{
        declared ? TRY(coerce_to(value, *declared, src, {.what = use_site::kind::STORE})) : value};
    const auto index{*comptime_value(stored)};
    auto&      type{type_of(stored)};
    const bool is_mutable{d.has(decl_flags::MUT)};
    auto&      global{comp_.gir().add_global(comp_.symbol_for(u_, d),
                                             type,
                                             !is_mutable,
                                             pool.value_at(index).data.is<ip::undefined_value>()
                                                 ? stdx::none
                                                 : stdx::option<gir::value>{constant(index, type)},
                                             compilation::linkage_of(d))};
    if (!is_mutable) { return stored; }
    return global_ref{&comp_.own_symbol(u_, decl, global.name), &type};
}

} // namespace ghoti::uir::sema
