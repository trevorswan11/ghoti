#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/id.hh"
#include "compiler/ip/index.hh"
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
    if (member.symbol_prefix.empty()) { return symbol_for(u, decl, u.uir->src_of(func)); }
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
    if (index >= captures_.size()) {
        return comp_.unsupported(u_, src_of(inst), "a capture of a runtime value");
    }

    return operand{captures_[index]};
}

auto analyzer::analyze_closure(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (!declaring_ || !is_comptime()) {
        return comp_.unsupported(u_, src, "a closure used as a value");
    }

    auto       rec{record(inst)};
    const auto func{as_inst(*rec.ref_field())};
    const auto count{rec.count()};
    auto       member{member_context_here()};
    member.captures.clear();
    for (u32 i{0}; i < count; ++i) {
        const auto value_ref{*rec.ref_field()};
        const auto by_ref{rec.u32_field()};
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

    return comp_.symbol_for(u_, decl, src);
}

auto analyzer::receiver(modifier how, gs::type& self_type, ref object_ref, const source& src)
    -> result<receiver_arg> {
    if (how == modifier::VALUE) {
        return receiver_arg{.value = TRY(resolve_value(object_ref, src)),
                            .mut   = gs::types::mut::CONSTANT};
    }

    // A compile-time evaluation would need comptime addresses, while a runtime body folding a
    // condition just keeps the call at runtime
    if (!gir_fn_) {
        return comp_.unsupported(u_, src, "a method taking its object's address at compile time");
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

    const auto object_place{TRY(address_of(object_ref, src))};
    const auto base{TRY(through_pointer(object_place, src))};

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
    return stdx::none;
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

        return call_generic(g, std::move(values), src, src);
    }

    const auto fn{member.as_opt<function_ref>()};
    if (!fn || !fn->fn->self_modifier) {
        return comp_.unsupported(u_, src, "a call of a member that isn't a method");
    }

    auto& method{*fn->fn};
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
    const auto result{TRY(call_with(method, std::move(values), src, false))};

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
