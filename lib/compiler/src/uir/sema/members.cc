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

#include "compiler/ast/id.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

using modifier = ast::type_modifier::modifier;

// The closure a declaration's value yields, if its value is one
[[nodiscard]] auto yielded_closure(const module_t& m, const decl& d) -> stdx::option<inst_id> {
    const auto body{m.body(d.value_body)};
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return stdx::none; }
    const auto yielded{as_inst(m.data_of(body.back()).un.operand)};
    if (!yielded || m.tag_of(*yielded) != tag::closure) { return stdx::none; }
    return yielded;
}

} // namespace

auto compilation::member_context_of(nominal& n) -> member_context {
    return member_context{
        .captures      = n.captures,
        .self_type     = *n.type,
        .symbol_prefix = n.symbol_prefix,
        .scope         = n,
    };
}

auto compilation::member_symbol(const unit&           u,
                                decl_id               decl,
                                inst_id               func,
                                const member_context& member) -> result<std::string> {
    if (!member.symbol.empty()) { return std::string{member.symbol}; }
    if (member.symbol_prefix.empty()) { return symbol_for(u, decl, u.uir->src_of(func), true); }
    return fmt::format("{}.{}", member.symbol_prefix, u.uir->string(u.uir->decl_at(decl).name));
}

auto compilation::self_param_type(const member_context& member, modifier how)
    -> stdx::option<gs::type&> {
    if (!member.self_type) { return stdx::none; }
    auto& self{*member.self_type};
    switch (how) {
    case modifier::VALUE:    return self;
    case modifier::REF:      return ctx_.get_reference(gs::types::mut::CONSTANT, self);
    case modifier::MUT_REF:  return ctx_.get_reference(gs::types::mut::MUTABLE, self);
    case modifier::PTR:      return ctx_.get_pointer(gs::types::mut::CONSTANT, self);
    case modifier::MUT_PTR:  return ctx_.get_pointer(gs::types::mut::MUTABLE, self);

    // `mut?` is one function whose `self` takes the mutability each call passes
    case modifier::POLY_REF: return ctx_.get_reference(gs::types::mut::POLY, self);
    case modifier::POLY_PTR: return ctx_.get_pointer(gs::types::mut::POLY, self);
    default:                 return stdx::none;
    }
}

auto analyzer::enter(nominal& n) -> void { enter(comp_.member_context_of(n)); }

auto analyzer::enter(const member_context& member) -> void {
    captures_      = member.captures;
    scope_         = member.scope;
    self_type_     = member.self_type;
    symbol_prefix_ = std::string{member.symbol_prefix};
}

auto analyzer::member_context_here() -> member_context {
    return member_context{
        .captures      = captures_,
        .self_type     = self_type_,
        .symbol_prefix = symbol_prefix_,
        .scope         = scope_,
    };
}

auto analyzer::analyze_capture(inst_id inst) -> result<operand> {
    const auto index{mod().data_of(inst).un.aux};
    if (index < env_values_.size() && env_values_[index]) { return *env_values_[index]; }

    // A pack captured only for its length is its elements' types
    for (const auto& [slot, values] : fn_ ? fn_->known_packs : decltype(fn_->known_packs){}) {
        if (slot != index) { continue; }
        std::vector<operand> elements;
        for (const auto value : values) { elements.emplace_back(value); }
        packs_.emplace_back(std::move(elements));
        return operand{pack_ref{static_cast<u32>(packs_.size() - 1)}};
    }
    if (fn_ && fn_->self_capture == index) { return function_ref{&*fn_}; }
    if (index >= captures_.size()) {
        return comp_.unsupported(u_, src_of(inst), "a capture of a runtime value");
    }

    return operand{captures_[index]};
}

auto analyzer::analyze_closure(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (!declaring_ || !is_comptime()) {
        if (fn_ && !declaring_) { return local_closure(inst); }
        return comp_.unsupported(u_, src, "a closure used as a value");
    }

    auto       rec{record(inst)};
    const auto func{as_inst(*rec.ref_field())};
    const auto count{rec.count()};
    auto       member{member_context_here()};
    member.captures.clear();
    for (u32 i{0}; i < count; ++i) {
        const auto value_ref{*rec.ref_field()};
        const auto by_ref{rec.u32_field() & flags::CAPTURE_BY_REF};
        rec.skip_str();
        const auto value{TRY(resolve_value(value_ref, src))};
        const auto index{comptime_value(value)};
        if (by_ref != 0 || !index) {
            return comp_.unsupported(u_, src, "a closure that captures a runtime value");
        }

        member.captures.emplace_back(*index);
    }

    return comp_.declare_function(u_, *std::exchange(declaring_, stdx::none), *func, member);
}

auto analyzer::closure_declares(inst_id func) const -> bool {
    if (!declaring_) { return false; }
    const auto closure{yielded_closure(mod(), mod().decl_at(*declaring_))};
    if (!closure) { return false; }
    record_reader rec{record(*closure)};
    return as_inst(*rec.ref_field()) == func;
}

auto analyzer::global_symbol(decl_id decl, const source& src) -> result<std::string> {
    if (!symbol_prefix_.empty()) {
        return fmt::format("{}.{}", symbol_prefix_, mod().string(mod().decl_at(decl).name));
    }

    return comp_.symbol_for(u_, decl, src, false);
}

auto analyzer::receiver(modifier how, gs::type& self_type, ref object_ref, const source& src)
    -> result<receiver_arg> {
    if (how == modifier::VALUE) {
        // A receiver named as a place or a mutable global is read from where it lives
        const auto object{TRY(resolve(object_ref, src))};
        const auto global{object.as_opt<global_ref>()};
        if (is_comptime() && (object.is<place>() || global)) {
            return comp_.needs_runtime(u_, src);
        }

        if (const auto p{object.as_opt<place>()}) {
            return receiver_arg{.value = TRY(read(*p, src)), .mut = gs::types::mut::CONSTANT};
        }

        if (global) {
            TRY(require_runtime(src));
            const auto at{global_place(global->symbol, *global->type, true)};
            return receiver_arg{.value = TRY(read(at, src)), .mut = gs::types::mut::CONSTANT};
        }

        return receiver_arg{.value = TRY(resolve_value(object_ref, src)),
                            .mut   = gs::types::mut::CONSTANT};
    }

    // Compile-time memory is passed by its compile-time address, which the method writes through
    if (const auto memory{TRY(memory_receiver(object_ref, src))}) {
        return receiver_arg{.value = memory_address(*memory, self_type),
                            .mut   = gs::types::mut::MUTABLE};
    }

    // At compile time a read-only method takes a reference holding the object's value
    if (!gir_fn_ || is_comptime()) {
        const bool pointer{how == modifier::PTR || how == modifier::POLY_PTR};
        const bool reads_only{how == modifier::REF || how == modifier::POLY_REF || pointer};
        // An object in runtime storage needs the call to run at runtime
        const auto stored{TRY(resolve(object_ref, src))};
        if (stored.is<place>()) { return comp_.needs_runtime(u_, src); }

        const auto object{TRY(resolve_value(object_ref, src))};
        const auto known{comptime_value(object)};
        if (!reads_only || !known) {
            return comp_.unsupported(
                u_, src, "a method taking its object's address at compile time");
        }

        auto&      pointee{*gs::pointee_of(self_type)};
        auto&      ref_type{pointer ? comp_.ctx().get_pointer(gs::types::mut::CONSTANT, pointee)
                                    : comp_.ctx().get_reference(gs::types::mut::CONSTANT, pointee)};
        const auto held{comptime_referent(object).value_or(*known)};
        return receiver_arg{.value = comp_.constants().intern(
                                ip::address_value{.symbol = {}, .pointee = {held}}, ref_type),
                            .mut = gs::types::mut::CONSTANT};
    }
    TRY(require_runtime(src));

    // A pointer or reference value already is the address the method takes
    const auto object{TRY(resolve(object_ref, src))};
    const auto direct{[&] -> stdx::option<gir::value> {
        if (const auto v{object.as_opt<gir::value>()}) { return *v; }
        if (const auto s{object.as_opt<slot_value>()}) { return s->origin; }
        return stdx::none;
    }()};
    if (direct && gs::pointee_of(type_of(object))) {
        return receiver_arg{.value = gir::value{direct->data, self_type},
                            .mut   = type_of(object).get_key().get_mut()};
    }

    // A receiver reached through a pointer is passed without checking it for null
    const auto object_place{TRY(address_of(object_ref, src))};
    const auto base{TRY(through_pointer(object_place, src, false))};

    // An object reached through a pointer or reference already has the address the method takes
    if (base.address.data != object_place.address.data) {
        return receiver_arg{.value = gir::value{base.address.data, self_type},
                            .mut   = place_mutability(base, object_place)};
    }

    const bool wants_mut{how == modifier::MUT_REF || how == modifier::MUT_PTR};
    if (wants_mut && !base.is_mutable) {
        return comp_.report(u_,
                            src,
                            gs::error::ASSIGNMENT_TO_CONST,
                            std::string{messages::MUTABLE_ADDRESS_OF_IMMUTABLE});
    }

    return receiver_arg{
        .value = gir::value{builder_.emit_address_of(base.address, self_type), self_type},
        .mut   = base.is_mutable ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT,
    };
}

auto analyzer::memory_receiver(ref object_ref, const source& src)
    -> result<stdx::option<comptime_place>> {
    const auto object{TRY(resolve(object_ref, src))};
    if (const auto through{memory_target(object)}) { return through; }
    if (const auto p{object.as_opt<comptime_place>()}) { return *p; }
    return stdx::none;
}

auto analyzer::self_modifier_of(const unit& u, inst_id func) const -> stdx::option<modifier> {
    const auto&   m{*u.uir};
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{params_of(m, rec.body_field())};
    if (params.empty()) { return stdx::none; }
    record_reader p{m, m.data_of(params.front()).pl.extra};
    p.skip_str().skip_body().skip_body();
    const auto param_flags{p.u32_field()};
    if ((param_flags & flags::PARAM_SELF) == 0) { return stdx::none; }
    return static_cast<modifier>(p.u32_field());
}

auto analyzer::receiver_type(const operand& object) -> stdx::option<gs::type&> {
    auto& type{type_of(object)};
    if (const auto pointee{gs::pointee_of(type)}) {
        if (comp_.nominal_of(*pointee)) { return *pointee; }
    }

    if (comp_.nominal_of(type)) { return type; }

    // A builtin type's methods come from the impls for it
    const auto impls{comp_.impls_of(type)};
    if (impls && !impls->empty()) { return type; }
    return stdx::none;
}

auto analyzer::interface_wrapper(gs::type&        owner,
                                 std::string_view name,
                                 usize            written,
                                 const source&    src) -> result<stdx::option<instance&>> {
    for (const auto applied : TRY(comp_.impls_of(owner))) {
        const auto iface{applied->interface ? comp_.nominal_of(*applied->interface) : stdx::none};
        if (!iface) { continue; }
        const auto wrapper{comp_.defaulted_method(*iface, name, written)};
        if (wrapper == name) { continue; }
        const auto member{TRY(comp_.impl_member(*applied, wrapper, src))};
        const auto fn{member ? member->as_opt<function_ref>() : stdx::none};
        if (fn && fn->fn->self_modifier) { return stdx::option<instance&>{*fn->fn}; }
    }

    return stdx::option<instance&>{};
}

auto analyzer::call_method(gs::type&            owner,
                           ref                  object_ref,
                           std::string_view     name,
                           gsl::span<const ref> args,
                           const source&        src) -> result<operand> {
    const auto member{TRY(member_of(owner, name, src))};
    if (const auto generic{member.as_opt<generic_ref>()}) {
        auto&      g{*generic->fn};
        const auto how{self_modifier_of(*g.owner, g.func)};
        const auto self{how ? comp_.self_param_type({.self_type = g.self_type}, *how)
                            : stdx::option<gs::type&>{}};
        if (!self) { return comp_.unsupported(u_, src, "a call of a member that isn't a method"); }

        std::vector<call_arg> values;
        values.emplace_back(call_arg{.value = TRY(receiver(*how, *self, object_ref, src)).value});
        for (const auto arg : args) {
            values.emplace_back(call_arg{.value = TRY(resolve_value(arg, src)), .written = arg});
        }

        if (!memory_place(values.front().value) || is_comptime()) {
            return call_generic(g, std::move(values), src, src);
        }

        const auto scope{comptime_scope()};
        return call_generic(g, std::move(values), src, src);
    }

    const auto fn{member.as_opt<function_ref>()};
    if (!fn || !fn->fn->self_modifier) {
        return comp_.unsupported(u_, src, "a call of a member that isn't a method");
    }

    // A call leaving trailing defaulted arguments off goes to the method's wrapper for them
    auto* chosen{fn->fn.get()};
    if (args.size() + 1 < chosen->params.size()) {
        if (const auto wrapper{
                TRY(default_wrapper(*chosen->owner, chosen->decl, args.size(), src))}) {
            if (const auto other{wrapper->as_opt<function_ref>()};
                other && other->fn->self_modifier) {
                chosen = other->fn.get();
            }
        }
    }

    // An impl's method takes its defaults from the interface, whose wrapper the impl inherits
    if (args.size() + 1 < chosen->params.size()) {
        if (const auto other{TRY(interface_wrapper(owner, name, args.size(), src))}) {
            chosen = &*other;
        }
    }

    auto& method{*chosen};
    if (args.size() + 1 != method.params.size()) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", method.params.size() - 1, args.size()));
    }

    const auto self{TRY(receiver(*method.self_modifier, *method.params.front(), object_ref, src))};
    std::vector<operand> values;
    values.reserve(method.params.size());
    values.emplace_back(self.value);
    for (const auto arg : args) { values.emplace_back(TRY(resolve_value(arg, src))); }

    // A method given compile-time memory runs at compile time, where alone it may write it
    const auto result{TRY([&] {
        if (!memory_place(self.value) || is_comptime()) {
            return call_with(method, std::move(values), src, false);
        }

        const auto scope{comptime_scope()};
        return call_with(method, std::move(values), src, false);
    }())};

    // A `mut?` method's result takes the mutability of the object it was called on
    const auto how{*method.self_modifier};
    if (how != modifier::POLY_REF && how != modifier::POLY_PTR) { return result; }
    const auto value{result.as_opt<gir::value>()};
    if (!value) { return result; }
    return gir::value{value->data, with_mutability(*value->type, self.mut)};
}

auto analyzer::place_mutability(const place& base, const place& object)
    -> gs::types::mut::mutability_modifiers {
    // Reached through a `mut?` pointer or reference, the object keeps that mutability
    if (object.pointee->is_poly()) { return gs::types::mut::POLY; }
    return base.is_mutable ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT;
}

auto analyzer::with_mutability(gs::type& type, gs::types::mut::mutability_modifiers mut)
    -> gs::type& {
    // Called on a `mut?` object, the result stays `mut?`
    if (!type.is_poly() || (mut & gs::types::mut::POLY) == gs::types::mut::POLY) { return type; }
    const auto pointee{gs::pointee_of(type)};
    if (!pointee) { return type; }
    auto& ctx{comp_.ctx()};
    return type.get_kind() == gs::type_kind::POINTER ? ctx.get_pointer(mut, *pointee)
                                                     : ctx.get_reference(mut, *pointee);
}

} // namespace ghoti::uir::sema
