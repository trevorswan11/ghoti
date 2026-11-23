#include <algorithm>
#include <bit>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"

namespace ghoti::uir::sema {

namespace {

// The well-known type a parameter's or return's type body yields directly, if any
[[nodiscard]] auto written_type(const module_t& m, gsl::span<const inst_id> body)
    -> stdx::option<well_known> {
    if (body.size() != 1 || m.tag_of(body.front()) != tag::yield) { return stdx::none; }
    return as_well_known(m.data_of(body.front()).un.operand);
}

// Whether a type written this way makes every call bind the parameter anew
[[nodiscard]] auto is_generic_type(stdx::option<well_known> w) noexcept -> bool {
    return w == well_known::auto_type || w == well_known::type_type ||
           w == well_known::comptime_int_type || w == well_known::comptime_float_type;
}

// Whether a parameter's type is an array whose length or element is one of the template's
// parameters, which a monomorph's symbol spells `type` when an untyped literal fills it
[[nodiscard]] auto sized_by_param(const module_t&          m,
                                  gsl::span<const inst_id> type_body,
                                  gsl::span<const inst_id> params) -> bool {
    if (type_body.empty() || m.tag_of(type_body.back()) != tag::yield) { return false; }
    const auto array{as_inst(m.data_of(type_body.back()).un.operand)};
    if (!array || m.tag_of(*array) != tag::array_type) { return false; }
    record_reader rec{m, m.data_of(*array).pl.extra};
    const auto    len{rec.ref_field().and_then(as_inst)};
    const auto    elem{as_inst(*rec.ref_field())};
    return (len && std::ranges::contains(params, *len)) ||
           (elem && std::ranges::contains(params, *elem));
}

// What a pointer, reference or slice holds, which an `auto` pattern matches against
[[nodiscard]] auto element_of(const gs::type& t) -> stdx::option<gs::type&> {
    if (const auto slice{t.get_data().as_opt<gs::types::slice>()}) { return slice->underlying; }

    return gs::pointee_of(t);
}

// Whether a type body spells `auto` behind pointers, references or slices: `&mut? auto`
[[nodiscard]] auto is_auto_pattern(const module_t& m, gsl::span<const inst_id> body) -> bool {
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return false; }
    auto at{m.data_of(body.back()).un.operand};
    while (const auto inst{as_inst(at)}) {
        const auto t{m.tag_of(*inst)};
        if (t != tag::ptr_type && t != tag::ref_type && t != tag::slice_type) { return false; }
        record_reader rec{m, m.data_of(*inst).pl.extra};
        at = *rec.ref_field();
    }

    return as_well_known(at) == well_known::auto_type;
}

// Whether a parameter's type is an `impl Fn(...)` bound, which binds whatever callable a call
// passes
[[nodiscard]] auto is_impl_fn(const module_t& m, gsl::span<const inst_id> body) -> bool {
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return false; }
    const auto inst{as_inst(m.data_of(body.back()).un.operand)};
    if (!inst || m.tag_of(*inst) != tag::fn_type) { return false; }
    record_reader rec{m, m.data_of(*inst).pl.extra};
    rec.skip_refs();
    const auto names{rec.count()};
    for (u32 i{0}; i < names; ++i) { rec.skip_str(); }
    rec.skip_ref().skip_u32();
    return (rec.u32_field() & flags::IMPL_FN) != 0;
}

[[nodiscard]] auto mut_prefix(const gs::type& underlying) -> std::string_view {
    return underlying.is_constant() ? "" : "mut_";
}

// The `impl` bounds a function record lists, read from where its attributes end
[[nodiscard]] auto impl_bounds_of(record_reader& rec) -> std::vector<impl_bound> {
    std::vector<impl_bound> bounds;
    const auto              count{rec.count()};
    for (u32 i{0}; i < count; ++i) {
        const auto param{rec.u32_field()};
        const auto interfaces{rec.refs_field()};
        bounds.emplace_back<impl_bound>({param, {interfaces.begin(), interfaces.end()}});
    }

    return bounds;
}

} // namespace

auto instance_key_hash::operator()(const instance_key& key) const noexcept -> u64 {
    stdx::hasher h{static_cast<u64>(std::bit_cast<uptr>(key.fn.get()))};
    for (const auto arg : key.args) { h.combine(std::to_underlying(arg)); }
    return h.finalize();
}

auto compilation::is_generic(const unit& u, inst_id func) const -> bool {
    const auto&   m{*u.uir};
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{params_of(m, rec.body_field())};
    const auto    ret{rec.body_field()};
    DISCARD(ret);
    rec.skip_body().skip_u32().skip_u32().skip_body();
    if (rec.count() != 0) { return true; }
    return std::ranges::any_of(params, [&](inst_id param) {
        record_reader p{m, m.data_of(param).pl.extra};
        p.skip_str();
        const auto type_body{p.body_field()};
        p.skip_body();
        const auto param_flags{p.u32_field()};
        return (param_flags & (flags::PARAM_COMPTIME | flags::PARAM_PACK)) != 0 ||
               is_generic_type(written_type(m, type_body)) || is_auto_pattern(m, type_body) ||
               is_impl_fn(m, type_body);
    });
}

auto compilation::declare_generic(const unit&           u,
                                  decl_id               decl,
                                  inst_id               func,
                                  const member_context& member) -> result<operand> {
    const auto& d{u.uir->decl_at(decl)};

    // A template's instances are numbered apart from a same-named template's, never qualified
    const bool in_instance{member.scope && member.scope->in_call && member.symbol.empty() &&
                           member.symbol_prefix == member.scope->symbol_prefix};
    const bool unprefixed{in_instance || (member.symbol.empty() && member.symbol_prefix.empty())};
    auto       name{unprefixed ? std::string{u.uir->string(d.name)}
                               : TRY(member_symbol(u, decl, func, member))};

    // An `impl` block's generic member is named after the type it implements
    const bool in_impl{d.owner && u.uir->tag_of(*d.owner) == tag::impl_decl && member.self_type};
    const auto enclosing{in_instance ? stdx::option<gs::type&>{*member.scope->type}
                         : in_impl   ? stdx::option<gs::type&>{*member.self_type}
                                     : stdx::none};
    auto&      fn{*generics_.emplace_back(arena_.make<generic_fn>(generic_fn{
        .owner     = &units_[u.index],
        .decl      = decl,
        .func      = func,
        .name      = std::move(name),
        .weak      = d.has(decl_flags::WEAK),
        .captures  = member.captures,
        .self_type = member.self_type,
        .scope     = member.scope,
        .enclosing = enclosing,
    }))};
    return generic_ref{&fn};
}

auto compilation::mangle_type(const gs::type& t) -> std::string {
    const auto& data{t.get_data()};
    switch (t.get_kind()) {
    case gs::type_kind::STRUCT:
    case gs::type_kind::UNION:
    case gs::type_kind::ENUM:   {
        const auto kind_name{gs::type_kind_display_name(t)};
        const auto n{nominal_of(t)};
        if (!n) { return std::string{kind_name}; }

        // A ctor instance's type is named for the call that made it
        if (n->in_call) {
            return fmt::format(
                "{}${}_{}", kind_name, n->symbol_prefix, std::to_underlying(n->inst));
        }

        // Instruction ids repeat across modules, so a nominal is numbered by its declaration
        return fmt::format("{}{}", kind_name, nominal_number(t));
    }
    case gs::type_kind::FUNCTION: {
        // A capturing generic's value is spelled by which generic it is
        if (const auto g{closure_generic(t)}; g && t.is_generic_value()) {
            return fmt::format("gfn{}", g->value_id);
        }

        const auto& fn{data.as<gs::types::function>()};
        return fmt::format(
            "{}_{}__{}",
            fn.erased ? "fn" : "xfn",
            fmt::join(fn.params | std::views::transform([&](auto p) { return mangle_type(*p); }),
                      "_"),
            mangle_type(fn.return_type));
    }
    case gs::type_kind::ARRAY: {
        const auto& arr{data.as<gs::types::array>()};
        return fmt::format("array{}{}_{}{}",
                           arr.len,
                           arr.null_terminated ? "z" : "",
                           mut_prefix(arr.underlying),
                           mangle_type(arr.underlying));
    }
    case gs::type_kind::VECTOR: {
        const auto& vec{data.as<gs::types::vector>()};
        return fmt::format("vector{}_{}", vec.len, mangle_type(vec.underlying));
    }
    case gs::type_kind::SLICE: {
        const auto& sl{data.as<gs::types::slice>()};
        return fmt::format("slice{}_{}{}",
                           sl.null_terminated ? "z" : "",
                           mut_prefix(sl.underlying),
                           mangle_type(sl.underlying));
    }
    case gs::type_kind::POINTER: {
        const auto& p{data.as<gs::types::pointer>()};
        return fmt::format("ptr_{}{}", mut_prefix(p.underlying), mangle_type(p.underlying));
    }
    case gs::type_kind::REFERENCE: {
        const auto& r{data.as<gs::types::reference>()};
        return fmt::format("ref_{}{}", mut_prefix(r.underlying), mangle_type(r.underlying));
    }
    default: return std::string{gs::type_kind_display_name(t)};
    }
}

auto compilation::versioned_name(const generic_fn& g) -> std::string {
    // Every instance of a type declares the same template, written once
    auto&      owners{generic_versions_[g.name]};
    const auto written{std::pair{g.owner->index, g.func}};
    const auto found{std::ranges::find(owners, written)};
    const auto index{static_cast<usize>(found - owners.begin())};
    if (found == owners.end()) { owners.emplace_back(written); }
    return index == 0 ? g.name : fmt::format("{}_v{}", g.name, index + 1);
}

auto compilation::instance_for(generic_fn& g, generic_binding binding)
    -> result<gsl::not_null<instance*>> {
    if (const auto found{instances_by_key_.find(binding.key)}; found != instances_by_key_.end()) {
        return found->second;
    }

    auto& ret{*binding.return_type};
    auto  span{ctx_.pool.get_many_unsafe(binding.slots.size())};
    for (usize i{0}; i < binding.slots.size(); ++i) { span[i] = binding.slots[i]; }
    auto& fn_type{ctx_.get_function(span, ret, false, ast::calling_convention::C, false)};

    auto symbol{fmt::format("{}__{}{}",
                            versioned_name(g),
                            fmt::join(binding.mangled_params, "_"),
                            fmt::join(binding.comptime_suffixes, ""))};
    if (g.enclosing) { symbol += fmt::format("_in_{}", mangle_type(*g.enclosing)); }
    symbol += g.suffix;

    // Bindings that mangle alike share one instance, such as `&T` and `&mut T` for a `&mut? auto`
    if (const auto same{instances_by_symbol_.find(symbol)}; same != instances_by_symbol_.end()) {
        instances_by_key_.emplace(std::move(binding.key), same->second);
        return same->second;
    }

    auto& fn{*instances_.emplace_back(arena_.make<instance>(instance{
        .owner       = g.owner,
        .decl        = g.decl,
        .func        = g.func,
        .symbol      = std::move(symbol),
        .type        = &fn_type,
        .params      = std::move(binding.slots),
        .return_type = &ret,
        .linkage     = gir::linkage::INTERNAL,
        .weak        = g.weak,
        .queued      = false,
        .comptime_only =
            ret.get_kind() == gs::type_kind::TYPE || gs::is_comptime_numeric(ret.get_kind()),
        .bindings      = std::move(binding.bindings),
        .infers_return = binding.infers_return,
        .source_name   = std::string{g.owner->uir->string(g.owner->uir->decl_at(g.decl).name)},
        .captures      = g.captures,
        .self_type     = g.self_type,
        .self_modifier = binding.self_modifier,
    }))};

    fn.comptime_env = g.comptime_env;
    fn.env_frame    = g.env_frame;

    // A local generic's instances take the environment its binding holds first
    if (g.env) {
        fn.env.emplace(*g.env);
        fn.type = &with_environment(fn_type, *g.env->type);
    }

    instances_by_key_.emplace(std::move(binding.key), &fn);
    instances_by_symbol_.emplace(fn.symbol, &fn);

    // Its body is emitted once a runtime call needs it, not for calls run at compile time
    return &fn;
}

auto analyzer::fitted_int_type(ip::index value) -> gs::type& {
    const auto big{int_of(comp_.constants().value_at(value))};
    auto&      ctx{comp_.ctx()};
    if (!big || big->fits(32, true)) { return ctx.get_int(32, true); }
    for (const auto [bits, is_signed] : {std::pair<u16, bool>{64, true},
                                         std::pair<u16, bool>{64, false},
                                         std::pair<u16, bool>{128, true},
                                         std::pair<u16, bool>{128, false}}) {
        if (big->fits(bits, is_signed)) { return ctx.get_int(bits, is_signed); }
    }

    return ctx.get_int(32, true);
}

auto analyzer::inferred_type(const operand& arg) -> gs::type& {
    // A generic function is a value of its own type
    if (const auto g{arg.as_opt<generic_ref>()}) {
        if (const auto value_type{comp_.generic_value_type(*g->fn)}) { return **value_type; }
    }

    auto& type{type_of(arg)};
    if (type.get_kind() == gs::type_kind::COMPTIME_INT) {
        if (const auto value{comptime_value(arg)}) { return fitted_int_type(*value); }
    }

    if (gs::is_comptime_numeric(type.get_kind())) { return comp_.ctx().default_concrete(type); }
    return type;
}

auto analyzer::bind_generic(generic_fn& g, gsl::span<const call_arg> args, const source& src)
    -> result<generic_binding> {
    const auto&   m{*g.owner->uir};
    record_reader rec{m, m.data_of(g.func).pl.extra};
    const auto    param_body{rec.body_field()};
    const auto    params{params_of(m, param_body)};
    const auto    ret{rec.body_field()};
    rec.skip_body().skip_u32().skip_u32().skip_body();
    const auto bounds{impl_bounds_of(rec)};

    // Parameter types are read in the template's scope, where earlier parameters are bound
    analyzer sig{comp_, *g.owner};
    sig.enter(member_context{.captures = g.captures, .self_type = g.self_type, .scope = g.scope});
    for (const auto inst : param_body) {
        if (m.tag_of(inst) != tag::param) { TRY(sig.prepare(inst)); }
    }

    generic_binding binding{.key = {.fn = &g, .args = {}}};
    for (usize i{0}; i < params.size(); ++i) {
        const auto    param{params[i]};
        record_reader p{m, m.data_of(param).pl.extra};
        p.skip_str();
        const auto  type_body{p.body_field()};
        const auto  default_body{p.body_field()};
        const auto  param_flags{p.u32_field()};
        const auto  modifier{p.u32_field()};
        const auto& param_src{m.src_of(param)};
        if (!default_body.empty()) {
            return comp_.unsupported(*g.owner, param_src, "a default argument");
        }

        if ((param_flags & flags::PARAM_SELF) != 0) {
            const auto how{static_cast<ast::type_modifier::modifier>(modifier)};
            const auto self{comp_.self_param_type({.self_type = g.self_type}, how)};
            if (!self) { return comp_.unsupported(*g.owner, param_src, "this `self` parameter"); }
            binding.self_modifier.emplace(how);
            binding.key.args.emplace_back(interned_type(*self));
            binding.bindings.emplace_back<param_binding>({.value = stdx::none, .has_slot = true});
            binding.slots.emplace_back(self.get());
            continue;
        }

        if (modifier != 0) {
            return comp_.unsupported(*g.owner, param_src, "a modified parameter");
        }

        // A pack takes every argument left, each in a GIR parameter of its own
        if ((param_flags & flags::PARAM_PACK) != 0) {
            if ((param_flags & flags::PARAM_COMPTIME) != 0 || i + 1 != params.size()) {
                return comp_.unsupported(*g.owner, param_src, "a comptime or leading pack");
            }

            const bool              untyped{type_body.empty() ||
                                            written_type(m, type_body) == well_known::auto_type};
            stdx::option<gs::type&> declared;
            if (!untyped) { declared.emplace(*TRY(sig.eval_type(type_body, param_src))); }
            for (usize j{i}; j < args.size(); ++j) {
                auto& type{declared ? *declared : inferred_type(args[j].value)};
                if (!gs::is_value_type(type.get_kind()) ||
                    gs::is_comptime_numeric(type.get_kind())) {
                    return comp_.unsupported(*g.owner, param_src, "a pack of comptime-only values");
                }

                binding.key.args.emplace_back(interned_type(type));
                binding.slots.emplace_back(&type);
                binding.mangled_params.emplace_back(comp_.mangle_type(type));
            }

            binding.bindings.emplace_back<param_binding>({
                .value     = stdx::none,
                .has_slot  = true,
                .pack_size = args.size() - i,
            });
            break;
        }

        const auto&             arg{args[i].value};
        const bool              is_auto{written_type(m, type_body) == well_known::auto_type ||
                                        is_impl_fn(m, type_body)};
        stdx::option<gs::type&> declared;
        if (!is_auto) { declared.emplace(*TRY(sig.eval_type(type_body, param_src))); }

        const bool comptime_numeric_arg{is_auto &&
                                        gs::is_comptime_numeric(type_of(arg).get_kind())};
        const bool binds_type{declared && declared->get_kind() == gs::type_kind::TYPE};
        if ((param_flags & flags::PARAM_COMPTIME) != 0 || binds_type || comptime_numeric_arg) {
            const use_site use{
                .what      = use_site::kind::ARGUMENT,
                .arg_index = i + 1,
                .callee    = g.name,
            };
            const auto index{TRY(comptime_argument(args[i], declared, use, src))};
            auto&      type{declared ? *declared : type_of(arg)};
            sig.bind(param, operand{index});
            binding.key.args.emplace_back(index);
            binding.bindings.emplace_back<param_binding>({.value = index, .has_slot = binds_type});
            if (binds_type) {
                // A type parameter keeps a slot of its own, which every call leaves undefined
                binding.slots.emplace_back(&type);
                binding.mangled_params.emplace_back(
                    comp_.mangle_type(comp_.constants().type_at(index)));
                continue;
            }

            // An array argument keeps its array type in the symbol
            const bool array_arg{!comp_.constants().is_type(index) &&
                                 type_of(arg).get_kind() == gs::type_kind::ARRAY};
            // A compile-time function keeps its own thin type, and is spelled by what it is
            const auto function{comp_.const_function(index)};
            auto& spelled{array_arg ? type_of(arg) : function ? type_of(operand{index}) : type};
            binding.mangled_params.emplace_back(comp_.mangle_type(spelled));
            binding.comptime_suffixes.emplace_back(fmt::format("_cx{}", comp_.mangle_value(index)));
            continue;
        }

        auto& type{declared ? *TRY(matched_type(*declared, arg, param_src)) : inferred_type(arg)};
        for (const auto& bound : bounds) {
            if (bound.param == i) { TRY(check_bound(sig, bound, type, g, param, args[i], src)); }
        }

        if (!gs::is_value_type(type.get_kind()) || gs::is_comptime_numeric(type.get_kind())) {
            return comp_.unsupported(*g.owner, param_src, "a parameter of a comptime-only type");
        }

        // A later type may ask a runtime parameter's type, as `@TypeOf(x)`, but never its value
        sig.bind(param, operand{gir::value{gir::local_id{}, type}});
        binding.key.args.emplace_back(interned_type(type));
        binding.bindings.emplace_back<param_binding>({.value = stdx::none, .has_slot = true});
        binding.slots.emplace_back(&type);
        // A function item or closure passed as an erased `fn` keeps its own type in the symbol
        const bool spelled_as_type{is_untyped_literal(arg) && sized_by_param(m, type_body, params)};
        const auto item{arg.as_opt<function_ref>()};
        const bool erased_param{type.get_kind() == gs::type_kind::FUNCTION};
        const auto closure{erased_param ? comp_.closure_body(type_of(arg)) : stdx::none};
        const auto arg_kind{type_of(arg).get_kind()};
        const bool array_for_slice{type.get_kind() == gs::type_kind::SLICE &&
                                   arg_kind == gs::type_kind::ARRAY};
        const auto pointee{gs::pointee_of(type_of(arg))};
        const bool function_ref_arg{erased_param && arg_kind == gs::type_kind::REFERENCE &&
                                    pointee && pointee->get_kind() == gs::type_kind::FUNCTION};
        std::string spelled{comp_.mangle_type(item && erased_param ? *item->fn->type
                                              : array_for_slice || function_ref_arg ? type_of(arg)
                                                                                    : type)};
        if (closure) {
            const auto& env{type_of(arg).get_data().as<gs::types::closure_t>()};
            spelled = fmt::format("{}_{}", closure->symbol, comp_.mangle_type(env.signature));
        }

        binding.mangled_params.emplace_back(spelled_as_type ? std::string{"type"}
                                                            : std::move(spelled));
    }

    if (written_type(m, ret) == well_known::auto_type) {
        binding.infers_return = true;
        binding.return_type.emplace(builtin(gs::type_kind::AUTO));
        return binding;
    }

    binding.return_type.emplace(*TRY(sig.eval_type(ret, m.src_of(g.func))));
    return binding;
}

auto analyzer::comptime_argument(const call_arg&         arg,
                                 stdx::option<gs::type&> declared,
                                 const use_site&         use,
                                 const source&           src) -> result<ip::index> {
    if (!declared && arg.value.is<enum_literal>()) {
        return comp_.unsupported(u_, src, "an enum literal bound to an untyped comptime parameter");
    }

    auto known{[&] -> result<operand> {
        const auto scope{comptime_scope()};
        auto       value{arg.value};

        // An argument built at runtime from constants is run again at compile time
        if (!comptime_value(value) && arg.written && !is_untyped_literal(value)) {
            value = TRY(reevaluate(*arg.written, src));
        }

        if (declared) { value = TRY(coerce_to(value, *declared, src, use)); }
        const auto known{TRY(require_comptime(value, src))};

        // A `comptime` parameter takes a function as a copy of it, like the old emitter
        if (const auto fn{comp_.plain_function(known)}) {
            return operand{TRY(comp_.comptime_function(*fn, {}))};
        }

        return operand{known};
    }()};
    if (is_comptime()) {
        if (!known) { return stdx::err{known.error()}; }
        return *comptime_value(*known);
    }

    const auto checked{TRY(at_comptime_boundary(std::move(known)))};
    return *comptime_value(checked);
}

auto analyzer::call_generic(generic_fn&              g,
                            std::vector<call_arg>    args,
                            const source&            src,
                            const source&            callee_src,
                            stdx::option<gir::value> env) -> result<operand> {
    PROFILE_FUNCTION();
    const auto&   m{*g.owner->uir};
    record_reader rec{m, m.data_of(g.func).pl.extra};
    const auto    params{params_of(m, rec.body_field())};
    const auto    param_count{params.size()};
    const bool    has_pack{std::ranges::any_of(params, [&](inst_id param) {
        record_reader p{m, m.data_of(param).pl.extra};
        p.skip_str().skip_body().skip_body();
        return (p.u32_field() & flags::PARAM_PACK) != 0;
    })};
    if (has_pack ? args.size() + 1 < param_count : args.size() != param_count) {
        return comp_.report(u_,
                            callee_src,
                            gs::error::ARITY_MISMATCH,
                            fmt::format("Expected {}{} arguments, found {}",
                                        has_pack ? "at least " : "",
                                        has_pack ? param_count - 1 : param_count,
                                        args.size()));
    }

    auto  binding{TRY(bind_generic(g, args, src))};
    auto& target{*TRY(comp_.instance_for(g, std::move(binding)))};

    // Each argument a parameter's slot takes, with a type parameter's left undefined
    std::vector<operand> values;
    for (usize i{0}; i < target.bindings.size(); ++i) {
        const auto& bound{target.bindings[i]};
        if (bound.pack_size) {
            for (usize j{i}; j < args.size(); ++j) {
                values.emplace_back(std::move(args[j].value));
            }
            break;
        }

        if (!bound.has_slot) { continue; }
        if (bound.value) {
            values.emplace_back(
                comp_.constants().intern(ip::undefined_value{}, builtin(gs::type_kind::TYPE)));
            continue;
        }

        values.emplace_back(std::move(args[i].value));
    }

    if (target.env && !env) { return comp_.unsupported(u_, src, "a local generic called by name"); }
    call_env_ = env;
    return call_following_poly(target, std::move(values), src, false);
}

auto analyzer::call_following_poly(instance&            target,
                                   std::vector<operand> values,
                                   const source&        src,
                                   bool                 through_member) -> result<operand> {
    // A `mut?` parameter's result takes the mutability of the argument the call passed it
    auto mut{gs::types::mut::POLY};
    for (usize i{0}; i < target.params.size() && i < values.size(); ++i) {
        if (target.params[i]->is_poly()) { mut = type_of(values[i]).get_key().get_mut(); }
    }

    const auto result{TRY(call_with(target, std::move(values), src, through_member))};
    const auto value{result.as_opt<gir::value>()};
    if (!value) { return result; }
    return gir::value{value->data, with_mutability(*value->type, mut)};
}

auto analyzer::matched_type(gs::type& declared, const operand& arg, const source& src)
    -> result<gsl::not_null<gs::type*>> {
    if (declared.get_kind() == gs::type_kind::AUTO) { return &inferred_type(arg); }

    // `[n]auto` takes the argument's element type, its length still checked by the coercion
    if (const auto arr{declared.get_data().as_opt<gs::types::array>()};
        arr && arr->underlying.get_kind() == gs::type_kind::AUTO) {
        const auto given{type_of(arg).get_data().as_opt<gs::types::array>()};
        if (!given) { return &declared; }
        return &comp_.ctx().get_array(
            declared.get_key().get_mut(), given->null_terminated, arr->len, given->underlying);
    }

    const auto leaf{element_of(declared)};
    if (!leaf || leaf->get_kind() != gs::type_kind::AUTO) { return &declared; }

    auto&      given{type_of(arg)};
    const auto given_leaf{element_of(given)};
    if (!given_leaf || given.get_kind() != declared.get_kind()) {
        return comp_.unsupported(u_, src, "an argument that doesn't match its `auto` pattern");
    }

    // `mut?` takes the argument's own mutability, any other modifier keeps the declared one
    const auto mut{declared.is_poly() ? given.get_key().get_mut() : declared.get_key().get_mut()};
    auto&      ctx{comp_.ctx()};
    switch (declared.get_kind()) {
    case gs::type_kind::POINTER: return &ctx.get_pointer(mut, *given_leaf);
    case gs::type_kind::SLICE:
        return &ctx.get_slice(
            mut, given.get_data().as<gs::types::slice>().null_terminated, *given_leaf);
    default: return &ctx.get_reference(mut, *given_leaf);
    }
}

auto analyzer::check_bound(analyzer&         sig,
                           const impl_bound& bound,
                           gs::type&         type,
                           const generic_fn& g,
                           inst_id           param,
                           const call_arg&   arg,
                           const source&     src) -> result<void> {
    const auto& m{*g.owner->uir};
    auto&       leaf{element_of(type).value_or(type)};
    for (const auto interface_ref : bound.interfaces) {
        const auto interface{TRY(sig.value_of(interface_ref, m.src_of(param)))};
        const auto index{comptime_value(interface)};
        auto&      pool{comp_.constants()};
        if (!index || !pool.is_type(*index) ||
            pool.type_at(*index).get_kind() != gs::type_kind::INTERFACE) {
            return comp_.unsupported(*g.owner, m.src_of(param), "a callable `impl` bound");
        }

        auto&      iface{pool.type_at(*index)};
        const auto impls{TRY(comp_.impls_of(leaf))};
        if (std::ranges::any_of(impls,
                                [&](const auto applied) { return applied->interface == iface; })) {
            continue;
        }

        record_reader p{m, m.data_of(param).pl.extra};
        const auto    name{p.str_field()};
        const auto    at{arg.written.and_then(as_inst)};
        return comp_.report(u_,
                            at ? src_of(*at) : src,
                            gs::error::UNSATISFIED_BOUND,
                            fmt::format("`{}` does not implement `{}` required by parameter `{}`",
                                        comp_.ctx().type_display_name(leaf),
                                        comp_.ctx().type_display_name(iface),
                                        name));
    }

    return {};
}

auto analyzer::bind_comptime_param(inst_id param, ip::index value) -> result<void> {
    auto&      pool{comp_.constants()};
    const bool aggregate{!pool.is_type(value) &&
                         (pool.value_at(value).data.is<ip::aggregate_value>() ||
                          pool.value_at(value).data.is<ip::union_value>())};
    if (!aggregate || holds_types(*pool.value_at(value).type)) {
        bind(param, operand{value});
        return {};
    }

    // A runtime body reads a comptime aggregate through a local built on entry
    const auto& src{src_of(param)};
    auto&       type{*comp_.constants().value_at(value).type};
    const auto  built{TRY(materialize(value, src))};
    const auto  slot{spill(built, type, true)};
    bind(param, slot_value{.address = slot.address, .type = &type, .known = value});
    return {};
}

auto params_of(const module_t& m, gsl::span<const inst_id> body) -> std::vector<inst_id> {
    std::vector<inst_id> params;
    for (const auto inst : body) {
        if (m.tag_of(inst) == tag::param) { params.emplace_back(inst); }
    }

    return params;
}

auto analyzer::prepare(inst_id inst) -> result<void> {
    values_.insert_or_assign(inst, TRY(analyze_inst(inst)));
    return {};
}

auto analyzer::value_of(ref r, const source& src) -> result<operand> { return resolve(r, src); }

auto analyzer::bind(inst_id param, operand value) -> void {
    values_.insert_or_assign(param, std::move(value));
}

} // namespace ghoti::uir::sema
