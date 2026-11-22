#include <string>
#include <utility>

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

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// The type a well-known type operand denotes, none for one this phase doesn't handle
[[nodiscard]] auto well_known_type(gs::context& ctx, well_known w) -> stdx::option<gs::type&> {
    const auto builtin{[&](gs::type_kind kind) -> stdx::option<gs::type&> {
        return ctx.get_builtin_resolved_type(kind);
    }};
    switch (w) {
    case well_known::u8_type:             return ctx.get_int(8, false);
    case well_known::i8_type:             return ctx.get_int(8, true);
    case well_known::u16_type:            return ctx.get_int(16, false);
    case well_known::i16_type:            return ctx.get_int(16, true);
    case well_known::u32_type:            return ctx.get_int(32, false);
    case well_known::i32_type:            return ctx.get_int(32, true);
    case well_known::u64_type:            return ctx.get_int(64, false);
    case well_known::i64_type:            return ctx.get_int(64, true);
    case well_known::u128_type:           return ctx.get_int(128, false);
    case well_known::i128_type:           return ctx.get_int(128, true);
    case well_known::usize_type:          return builtin(gs::type_kind::USIZE);
    case well_known::isize_type:          return builtin(gs::type_kind::ISIZE);
    case well_known::f16_type:            return builtin(gs::type_kind::F16);
    case well_known::f32_type:            return builtin(gs::type_kind::F32);
    case well_known::f64_type:            return builtin(gs::type_kind::F64);
    case well_known::f80_type:            return builtin(gs::type_kind::F80);
    case well_known::f128_type:           return builtin(gs::type_kind::F128);
    case well_known::bool_type:           return builtin(gs::type_kind::BOOL);
    case well_known::type_type:           return builtin(gs::type_kind::TYPE);
    case well_known::void_type:           return builtin(gs::type_kind::VOID_);
    case well_known::noreturn_type:       return builtin(gs::type_kind::NORETURN);
    case well_known::comptime_int_type:   return builtin(gs::type_kind::COMPTIME_INT);
    case well_known::comptime_float_type: return builtin(gs::type_kind::COMPTIME_FLOAT);
    case well_known::opaque_type:         return builtin(gs::type_kind::OPAQUE);
    case well_known::nullptr_type:        return builtin(gs::type_kind::NULLPTR);
    case well_known::auto_type:           return builtin(gs::type_kind::AUTO);
    default:                              return stdx::none;
    }
}

} // namespace

analyzer::analyzer(compilation& comp, const unit& u, stdx::option<decl_id> declaring) noexcept
    : comp_{comp}, u_{u}, declaring_{declaring}, frame_{comp.next_frame()},
      alloc_base_{comp.alloc_count()} {}

analyzer::analyzer(compilation& comp, const unit& u, instance& fn, gir::function& gir_fn) noexcept
    : comp_{comp}, u_{u}, fn_{fn}, gir_fn_{gir_fn}, captures_{fn.captures}, scope_{fn.scope},
      self_type_{fn.self_type}, frame_{comp.next_frame()}, alloc_base_{comp.alloc_count()} {}

analyzer::~analyzer() { comp_.release_allocs(alloc_base_); }

auto analyzer::eval_comptime(gsl::span<const inst_id> body) -> result<operand> {
    const auto scope{comptime_scope()};
    const auto out{TRY(analyze_body(body))};
    if (jump_) {
        return comp_.unsupported(u_, src_of(body.back()), "a jump out of a comptime body");
    }

    if (!out.yielded) {
        return comp_.unsupported(u_, src_of(body.back()), "a comptime body that doesn't yield");
    }

    return *out.yielded;
}

auto analyzer::eval_type(gsl::span<const inst_id> body, const source& src)
    -> result<gsl::not_null<gs::type*>> {
    if (body.empty()) { return comp_.unsupported(u_, src, "an inferred type"); }
    const auto value{TRY(eval_comptime(body))};
    return denoted_type(value, src);
}

auto analyzer::analyze_function() -> result<void> {
    PROFILE_FUNCTION();
    ASSERT(fn_ && gir_fn_, "a function analysis has a function");
    builder_.set_insert_point(*gir_fn_, gir_fn_->add_segment());

    record_reader rec{record(fn_->func)};
    const auto    params{params_of(mod(), rec.body_field())};
    rec.skip_body();
    const auto body{rec.body_field()};
    rec.skip_u32().skip_u32();
    const auto attrs{rec.body_field()};
    for (usize i{0}, slot{0}; const auto param : params) {
        const auto binding{fn_->bindings.empty() ? stdx::option<param_binding>{}
                                                 : stdx::option<param_binding>{fn_->bindings[i]}};
        ++i;
        if (binding && binding->value) { TRY(bind_comptime_param(param, *binding->value)); }
        if (binding && !binding->has_slot) { continue; }

        record_reader p{record(param)};
        const auto    name{p.str_field()};
        p.skip_body().skip_body();
        const auto param_flags{p.u32_field()};
        if (binding && binding->pack_size) {
            std::vector<operand> elements;
            for (usize k{0}; k < *binding->pack_size; ++k) {
                auto&       type{*fn_->params[slot++]};
                const auto& added{gir_fn_->add_param(fmt::format("{}#{}", name, k), type)};
                elements.emplace_back(gir::value{added.id, type});
            }

            values_.insert_or_assign(param, pack_ref{static_cast<u32>(packs_.size())});
            packs_.emplace_back(std::move(elements));
            continue;
        }

        auto&       type{*fn_->params[slot++]};
        const bool  discarded{(param_flags & flags::PARAM_DISCARDED) != 0 || name == "_"};
        const auto& added{gir_fn_->add_param(discarded ? std::string{} : std::string{name}, type)};
        const gir::value value{added.id, type};
        if (binding && binding->value) { continue; }

        // A slice parameter is passed in a slot of its own, and a written one is spilled to one
        if (type.get_kind() == gs::type_kind::SLICE) {
            values_.insert_or_assign(param, slot_value{.address = value, .type = &type});
        } else if ((param_flags & flags::PARAM_WRITTEN) != 0) {
            const auto spilled{spill(value, type, true)};
            values_.insert_or_assign(
                param, slot_value{.address = spilled.address, .type = &type, .is_mutable = true});
        } else {
            values_.insert_or_assign(param, value);
        }
    }

    // Attribute arguments may read the comptime parameters bound above
    if (!fn_->attributes) {
        const auto& d{mod().decl_at(fn_->decl)};
        fn_->attributes = TRY(fold_attributes(
            attrs,
            {.target       = ast::attribute_target::FN,
             .returns_void = fn_->return_type->get_kind() == gs::type_kind::VOID_,
             .declared_by  = declares_literal(mod(), d, fn_->func) ? stdx::option<const decl&>{d}
                                                                   : stdx::none}));
    }

    gir_fn_->set_attributes(*fn_->attributes);

    // `@setEval*Limit` in the body scopes over the rest of it
    const auto saved{comp_.limits()};
    const auto restore{gsl::finally([&] { comp_.limits() = saved; })};
    const auto out{analyze_body(body)};
    if (!out && out.error() == stop::UNSUPPORTED) { return stdx::err{stop::UNSUPPORTED}; }
    if (terminated()) { return {}; }

    // An inferred return type with no `return` to fix it is `void`
    if (fn_->infers_return && fn_->return_type->get_kind() == gs::type_kind::AUTO) {
        fn_->return_type = &builtin(gs::type_kind::VOID_);
    }

    auto& ret{*fn_->return_type};
    if (ret.get_kind() == gs::type_kind::VOID_) {
        builder_.emit_return();
    } else {
        builder_.emit_return(constant(comp_.constants().intern(ip::undefined_value{}, ret), ret));
    }

    return {};
}

auto analyzer::analyze_body(gsl::span<const inst_id> body) -> result<flow> {
    for (const auto inst : body) {
        if (mod().tag_of(inst) == tag::str) { continue; }
        if (mod().tag_of(inst) == tag::yield) {
            const auto& d{mod().data_of(inst)};
            return flow{.yielded = TRY(resolve(d.un.operand, src_of(inst)))};
        }

        if (!is_comptime()) { start_bool_ops(inst); }
        auto value{analyze_inst(inst)};
        if (!value) {
            // A failed instruction poisons only what uses it, so later root causes still report
            if (value.error() == stop::UNSUPPORTED || is_comptime()) {
                return stdx::err{value.error()};
            }

            continue;
        }

        // Where a call returned a runtime reference, for a read of it that must follow at once
        const bool is_call{mod().tag_of(inst) == tag::call ||
                           mod().tag_of(inst) == tag::field_call};
        if (const auto made{value->as_opt<gir::value>()};
            is_call && made && made->type && made->type->get_kind() == gs::type_kind::REFERENCE &&
            !is_comptime()) {
            reference_segments_.insert_or_assign(inst, builder_.get_segment()->get_id());
        }

        values_.insert_or_assign(inst, std::move(*value));
        if (jump_) { return flow{}; }
        if (!is_comptime() && terminated()) { return flow{}; }
    }

    return flow{};
}

auto analyzer::analyze_inst(inst_id inst) -> result<operand> {
    const auto t{mod().tag_of(inst)};
    set_location(src_of(inst));
    switch (t) {
    case tag::int_value:       return analyze_int_value(inst);
    case tag::int_lit:         return analyze_int_lit(inst);
    case tag::float_lit:       return analyze_float_lit(inst);
    case tag::int_type:        return analyze_int_type(inst);
    case tag::func:            return analyze_func(inst);
    case tag::capture:         return analyze_capture(inst);
    case tag::closure:         return analyze_closure(inst);

    case tag::add:
    case tag::sub:
    case tag::mul:
    case tag::div:
    case tag::mod:
    case tag::add_wrap:
    case tag::sub_wrap:
    case tag::mul_wrap:
    case tag::add_sat:
    case tag::sub_sat:
    case tag::mul_sat:
    case tag::shl_sat:
    case tag::shl_wrap:
    case tag::shl:
    case tag::shr:
    case tag::bit_and:
    case tag::bit_or:
    case tag::bit_xor:
    case tag::cmp_eq:
    case tag::cmp_ne:
    case tag::cmp_lt:
    case tag::cmp_le:
    case tag::cmp_gt:
    case tag::cmp_ge:          return analyze_binary(inst, t);
    case tag::bool_and:        return analyze_bool_op(inst, true);
    case tag::bool_or:         return analyze_bool_op(inst, false);
    case tag::neg:
    case tag::neg_wrap:
    case tag::pos:
    case tag::bool_not:
    case tag::bit_not:         return analyze_unary(inst, t);

    case tag::decl_val:        return analyze_decl_val(inst, false);
    case tag::decl_ref:        return analyze_decl_val(inst, true);
    case tag::import:          return analyze_import(inst);
    case tag::alloc_mut:       return analyze_alloc_mut(inst, false);
    case tag::alloc_comptime:  return analyze_alloc_mut(inst, true);
    case tag::let_value:       return analyze_let_value(inst);
    case tag::const_local:     return analyze_const_local(inst);
    case tag::store:           return analyze_store(inst);
    case tag::load:            return analyze_load(inst);
    case tag::discard:         return analyze_discard(inst);

    case tag::block:           return analyze_block(inst);
    case tag::block_comptime:  return analyze_block_comptime(inst);
    case tag::condbr_comptime: return analyze_condbr_comptime(inst);
    case tag::eval_branch:     return analyze_eval_branch(inst);
    case tag::unreachable:     return analyze_unreachable(inst);
    case tag::builtin:         return analyze_builtin(inst);
    case tag::type_of:         return analyze_type_of(inst);
    case tag::loop:            return analyze_loop(inst);
    case tag::condbr:          return analyze_condbr(inst);
    case tag::break_to:        return analyze_break(inst);
    case tag::repeat:          return analyze_repeat(inst);
    case tag::ret:             return analyze_ret(inst);
    case tag::ret_value:       return analyze_ret_value(inst);
    case tag::mem_view:        return analyze_mem_view(inst);
    case tag::concat:          return analyze_concat(inst);
    case tag::unwrap:          return analyze_unwrap(inst);
    case tag::merge_enums:     return analyze_merge_enums(inst);
    case tag::unwrap_branch:   return analyze_unwrap_branch(inst);
    case tag::residual:        return analyze_residual(inst);
    case tag::ret_residual:    return analyze_ret_residual(inst);
    case tag::call:            return analyze_call(inst);
    case tag::field_call:      return analyze_field_call(inst);

    case tag::ptr_type:
    case tag::ref_type:
    case tag::slice_type:      return analyze_elem_type(inst, t);
    case tag::array_type:      return analyze_array_type(inst);
    case tag::fn_type:         return analyze_fn_type(inst);
    case tag::struct_decl:     return analyze_nominal_decl(inst, gs::type_kind::STRUCT);
    case tag::union_decl:      return analyze_nominal_decl(inst, gs::type_kind::UNION);
    case tag::enum_decl:       return analyze_nominal_decl(inst, gs::type_kind::ENUM);
    case tag::interface_decl:  return analyze_interface_decl(inst);
    case tag::dyn_type:        return analyze_dyn_type(inst);

    case tag::implicit_access: return enum_literal{inst};
    case tag::init_struct:     return analyze_init_struct(inst);
    case tag::init_array:      return analyze_init_array(inst);
    case tag::array_lit:       return analyze_array_lit(inst);
    case tag::field_val:       return analyze_field_val(inst);
    case tag::field_ptr:       return analyze_field_ptr(inst);
    case tag::elem_val:        return analyze_elem_val(inst);
    case tag::elem_ptr:        return analyze_elem_ptr(inst);
    case tag::ref_of:          return analyze_ref_of(inst, false);
    case tag::address_of:      return analyze_ref_of(inst, true);
    case tag::deref:           return analyze_deref(inst);
    case tag::match:           return analyze_match(inst);
    case tag::slice:           return analyze_slice(inst);
    case tag::range:           return analyze_range(inst);
    case tag::for_iter:        return analyze_for(inst);
    case tag::bind:            return analyze_bind(inst);

    default:                   break;
    }

    return comp_.unsupported(u_, src_of(inst), fmt::format("the `{}` instruction", tag_name(t)));
}

auto analyzer::resolve(ref r, const source& src) -> result<operand> {
    if (const auto w{as_well_known(r)}) { return well_known_operand(*w, src); }
    const auto inst{*as_inst(r)};
    const auto found{values_.find(inst)};

    // A string literal is a constant `[N:0]u8`
    if (found == values_.end() && std::to_underlying(inst) < mod().instruction_count() &&
        mod().tag_of(inst) == tag::str) {
        const auto text{mod().string(mod().data_of(inst).str)};
        auto&      ctx{comp_.ctx()};
        auto&      type{
            ctx.get_array(gs::types::mut::CONSTANT, true, text.size(), ctx.get_int(8, false))};
        return comp_.constants().intern(std::string{text}, type);
    }

    if (found == values_.end()) {
        // A failed instruction left no value, and its failure was already reported
        if (std::to_underlying(inst) < mod().instruction_count() && !is_comptime()) {
            return stdx::err{stop::POISON};
        }

        return comp_.unsupported(u_, src, "a value from another body");
    }

    // A comptime read of a runtime value runs what computed it again, at compile time
    if (is_comptime() && (found->second.is<gir::value>() || found->second.is<merge_ref>() ||
                          found->second.is<slot_value>())) {
        return reevaluate(r, src);
    }

    // An aggregate global is its value at compile time
    if (const auto g{found->second.as_opt<global_const>()}; g && is_comptime()) { return g->value; }
    return found->second;
}

auto analyzer::resolve_value(ref r, const source& src) -> result<operand> {
    const auto op{TRY(resolve(r, src))};
    if (op.is<place>() || op.is<global_ref>() || op.is<comptime_place>()) {
        return comp_.unsupported(u_, src, "a place used as a value");
    }

    return op;
}

auto analyzer::resolve_type(ref r, const source& src) -> result<gsl::not_null<gs::type*>> {
    const auto op{TRY(resolve(r, src))};
    return denoted_type(op, src);
}

auto analyzer::well_known_operand(well_known w, const source& src) -> result<operand> {
    auto& ctx{comp_.ctx()};
    auto& pool{comp_.constants()};
    if (const auto type{well_known_type(ctx, w)}) { return interned_type(*type); }
    switch (w) {
    case well_known::bool_true:  return pool.intern(true, builtin(gs::type_kind::BOOL));
    case well_known::bool_false: return pool.intern(false, builtin(gs::type_kind::BOOL));
    case well_known::void_value:
        return pool.intern(ip::void_value{}, builtin(gs::type_kind::VOID_));
    case well_known::undefined:
        return pool.intern(ip::undefined_value{}, builtin(gs::type_kind::UNDEFINED));
    case well_known::nullptr_value:
        return pool.intern(ip::nullptr_value{}, builtin(gs::type_kind::NULLPTR));
    case well_known::builtin_module:
        if (const auto prelude{comp_.prelude_module()}) { return *prelude; }
        break;
    default: break;
    }

    return comp_.unsupported(u_, src, fmt::format("`{}`", well_known_name(w)));
}

auto analyzer::comptime_value(const operand& op) const -> stdx::option<ip::index> {
    if (const auto index{op.as_opt<ip::index>()}) { return *index; }
    return stdx::none;
}

auto analyzer::type_of(const operand& op) -> gs::type& {
    auto& pool{comp_.constants()};
    return op.visit(
        [&](ip::index i) -> gs::type& {
            if (pool.is_type(i)) { return builtin(gs::type_kind::TYPE); }
            return pool.value_at(i).type.value_or(builtin(gs::type_kind::VOID_));
        },
        [](const gir::value& v) -> gs::type& { return *v.type; },
        [](const function_ref& f) -> gs::type& { return *f.fn->type; },
        [&](const generic_ref&) -> gs::type& { return builtin(gs::type_kind::AUTO); },
        [](const place& p) -> gs::type& { return *p.pointee; },
        [&](merge_ref m) -> gs::type& { return type_of(merges_[m.index].arrivals.front().value); },
        [](const global_ref& g) -> gs::type& { return *g.type; },
        [&](comptime_place p) -> gs::type& {
            return comp_.alloc_at(p).type.value_or(builtin(gs::type_kind::UNDEFINED));
        },
        [](const slot_value& s) -> gs::type& { return *s.type; },
        [&](anon_init) -> gs::type& { return builtin(gs::type_kind::UNDEFINED); },
        [&](enum_literal) -> gs::type& { return builtin(gs::type_kind::ENUM_LITERAL); },
        [](const global_const& g) -> gs::type& { return *g.type; },
        [&](const module_ref&) -> gs::type& { return builtin(gs::type_kind::VOID_); },
        [&](cast_ref) -> gs::type& { return builtin(gs::type_kind::UNDEFINED); },
        [&](pack_ref) -> gs::type& { return builtin(gs::type_kind::VOID_); },
        [&](implicit_call) -> gs::type& { return builtin(gs::type_kind::UNDEFINED); });
}

auto analyzer::denoted_type(const operand& op, const source& src)
    -> result<gsl::not_null<gs::type*>> {
    const auto index{comptime_value(op)};
    if (!index || !comp_.constants().is_type(*index)) {
        return comp_.unsupported(u_, src, "a type position holding a value");
    }

    return &comp_.constants().type_at(*index);
}

auto analyzer::interned_type(gs::type& t) -> ip::index { return t.get_ip_index(); }

} // namespace ghoti::uir::sema
