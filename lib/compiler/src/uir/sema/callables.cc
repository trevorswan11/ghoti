#include <algorithm>
#include <stdx/utility.hh>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "gsl/pointers"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

constexpr u64 CLOSURE_MARKER{0x7569725f636c6f73};
constexpr u64 GENERIC_VALUE_MARKER{0x7569725f67656e76};

// Small values an environment holds by value, anything else it borrows
[[nodiscard]] auto captured_by_value(const gs::type& type) noexcept -> bool {
    const auto kind{type.get_kind()};
    return gs::is_numeric(kind) || kind == gs::type_kind::BOOL || kind == gs::type_kind::POINTER ||
           kind == gs::type_kind::FUNCTION || kind == gs::type_kind::CLOSURE;
}

// Whether the closure captures any place by reference
[[nodiscard]] auto captures_by_ref(const module_t& m, inst_id closure) -> bool {
    record_reader rec{m, m.data_of(closure).pl.extra};
    rec.skip_ref();
    const auto count{rec.count()};
    bool       any{false};
    for (u32 i{0}; i < count; ++i) {
        rec.skip_ref();
        any = any || (rec.u32_field() & flags::CAPTURE_BY_REF) != 0;
        rec.skip_str();
    }

    return any;
}

// Whether the function literal is a `move fn`, which owns copies of what it captures
[[nodiscard]] auto is_move_fn(const module_t& m, inst_id func) -> bool {
    record_reader rec{m, m.data_of(func).pl.extra};
    rec.skip_body().skip_body().skip_body().skip_u32();
    return (rec.u32_field() & flags::MOVE) != 0;
}

} // namespace

auto compilation::defer_body(instance& fn) -> void {
    if (queue_.empty() || queue_.back() != &fn) { return; }
    fn.queued = false;
    queue_.pop_back();
}

auto compilation::local_function_of(const unit&                u,
                                    inst_id                    inst,
                                    const instance*            enclosing,
                                    gsl::span<const ip::index> captures) -> stdx::option<operand> {
    for (const auto& known : local_fns_) {
        if (known.unit_index == u.index && known.inst == inst && known.enclosing == enclosing &&
            std::ranges::equal(known.captures, captures)) {
            return known.fn;
        }
    }

    return stdx::none;
}

auto compilation::declare_local(const unit& u, const instance& enclosing, local_decl decl)
    -> result<operand> {
    const bool  is_closure{!decl.env_captures.empty()};
    const bool  generic{is_generic(u, decl.func)};
    const auto& src{u.uir->src_of(decl.func)};
    if (decl.self_slot && !generic && is_closure) {
        return unsupported(u, src, "a closure that names itself");
    }

    // A generic is named for its binding, which its instances' symbols start with
    const auto        id{local_count_++};
    const std::string symbol{generic      ? std::string{decl.name}
                             : is_closure ? fmt::format("closure{}", id)
                                          : fmt::format("localfn.{}", id)};
    decl.member.symbol = lasting_name(symbol);
    const auto declared{TRY(declare_function(u, enclosing.decl, decl.func, decl.member))};

    // A local function's body is emitted once a runtime use needs it
    if (const auto fn{declared.as_opt<function_ref>()}) {
        fn->fn->known_packs = decl.known_packs;
        defer_body(*fn->fn);
    }

    // A generic's or a ctor instance's function declares its own local generics, named apart
    const auto owner{enclosing.scope       ? stdx::option<nominal&>{*enclosing.scope}
                     : enclosing.self_type ? nominal_of(*enclosing.self_type)
                                           : stdx::none};
    const bool in_ctor{owner && owner->in_call};
    if (const auto g{declared.as_opt<generic_ref>()};
        g && (!enclosing.bindings.empty() || in_ctor)) {
        g->fn->suffix = fmt::format("_i{:016x}", id);
        if (in_ctor) { g->fn->suffix += fmt::format("_in_{}", mangle_type(*owner->type)); }
    }

    local_fns_.emplace_back<local_fn>({
        .unit_index = u.index,
        .inst       = decl.inst,
        .enclosing  = &enclosing,
        .captures   = decl.member.captures,
        .fn         = declared,
    });
    if (!is_closure) {
        if (decl.self_slot && generic) {
            return unsupported(u, src, "a generic that names itself and captures nothing");
        }

        if (const auto fn{declared.as_opt<function_ref>()}) {
            fn->fn->self_capture = decl.self_slot;
        }
        return declared;
    }

    // The environment is a type of its own, which the body takes by `&mut` first
    const gs::types::key_t key{
        gs::type_kind::CLOSURE, gs::types::mut::CONSTANT, CLOSURE_MARKER, u64{id}};
    auto& env_type{*ctx_.pool[key]};
    auto  captures{
        make_uninit_span<gs::types::closure_capture>(ctx_.arena, decl.env_captures.size())};
    std::ranges::copy(decl.env_captures, captures.begin());
    auto& env{*arena_.make<closure_env>(closure_env{
        .type       = &env_type,
        .fields     = std::move(decl.fields),
        .pack_slots = std::move(decl.pack_slots),
        .moves      = is_move_fn(*u.uir, decl.func),
        .self_name  = generic ? "env" : "self",
        .self_slot  = decl.self_slot,
        .id         = id,
    })};

    // A generic's binding holds the environment under a function type of the generic's own
    if (const auto g{declared.as_opt<generic_ref>()}) {
        if (!decl.signature) { return unsupported(u, src, "a local generic with no signature"); }
        auto& signature{*decl.signature};
        env_type.resolve<gs::types::closure_t>(captures, signature, signature);
        auto& value_type{new_generic_value(*g->fn, signature)};
        value_type.set_generic_env(env_type);
        env.value_type.emplace(value_type);
        g->fn->env.emplace(env);
        closure_generics_.emplace(&env_type, g->fn);
        return declared;
    }

    auto& fn{*declared.as<function_ref>().fn};
    if (fn.infers_return) { return unsupported(u, src, "a closure whose return type is inferred"); }

    auto& signature{*fn.type};
    auto& impl{with_environment(signature, env_type)};
    env_type.resolve<gs::types::closure_t>(captures, signature, impl);
    fn.type = &impl;
    fn.env.emplace(env);
    closure_bodies_.emplace(&env_type, &fn);
    return declared;
}

auto compilation::new_generic_value(generic_fn& g, gs::type& signature) -> gs::type& {
    const auto& sig{signature.get_data().as<gs::types::function>()};
    const auto  id{local_count_++};
    auto&       value_type{*ctx_.pool[gs::types::key_t{
        gs::type_kind::FUNCTION, gs::types::mut::CONSTANT, GENERIC_VALUE_MARKER, u64{id}}]};
    value_type.resolve<gs::types::function>(sig.params, sig.return_type, false, false, sig.conv);
    value_type.mark_generic_value();
    g.value_type.emplace(value_type);
    g.value_id = id;
    closure_generics_.emplace(&value_type, &g);
    return value_type;
}

auto compilation::generic_value_type(generic_fn& g) -> result<gsl::not_null<gs::type*>> {
    if (g.value_type) { return &*g.value_type; }
    analyzer   sig{*this, *g.owner};
    const auto signature{TRY(sig.local_generic_signature(g.func, {}))};
    return &new_generic_value(g, *signature);
}

auto compilation::with_environment(gs::type& signature, gs::type& env_type) -> gs::type& {
    const auto& sig{signature.get_data().as<gs::types::function>()};
    auto        params{ctx_.pool.get_many_unsafe(sig.params.size() + 1)};
    params[0] = &ctx_.get_reference(gs::types::mut::MUTABLE, env_type);
    for (usize i{0}; i < sig.params.size(); ++i) { params[i + 1] = sig.params[i]; }
    return ctx_.get_function(params, sig.return_type, false, sig.conv, false);
}

auto compilation::comptime_function(const instance& fn, gsl::span<const named_capture> captures)
    -> result<ip::index> {
    const auto& u{*fn.owner};
    const auto  node{u.uir->src_of(fn.func).as_opt<ast::node_id>()};
    if (!node) { return unsupported(u, u.uir->src_of(fn.func), "a compile-time function value"); }
    std::vector<std::string> parts;
    for (const auto& [name, value] : captures) {
        parts.emplace_back(fmt::format(
            "{}.{}", name, gir::const_value::from_interned(constants(), value).mangle()));
    }

    std::ranges::sort(parts);
    const auto symbol{
        fmt::format("closureconst.cl.{}.{}", node->get_index(), fmt::join(parts, "."))};
    if (!const_functions_.contains(symbol)) {
        member_context member{
            .captures  = fn.captures,
            .self_type = fn.self_type,
            .scope     = fn.scope,
            .symbol    = lasting_name(symbol),
        };
        const auto declared{TRY(declare_function(u, fn.decl, fn.func, member))};
        const auto copy{declared.as_opt<function_ref>()};
        if (!copy) {
            return unsupported(u, u.uir->src_of(fn.func), "a compile-time generic value");
        }
        // A global may hold the copy, so its body is always emitted
        const_functions_.emplace(symbol, copy->fn);
    }

    return constants().intern(symbol, *fn.type);
}

auto compilation::named_function(const function_names& names, ip::index value)
    -> stdx::option<instance&> {
    if (constants().is_type(value)) { return stdx::none; }
    const auto text{constants().value_at(value).data.as_opt<std::string>()};
    if (!text) { return stdx::none; }
    const auto found{names.find(*text)};
    if (found == names.end()) { return stdx::none; }
    return *found->second;
}

auto compilation::const_function(ip::index value) -> stdx::option<instance&> {
    if (const auto copy{named_function(const_functions_, value)}) { return copy; }
    return plain_function(value);
}

auto compilation::function_value(instance& fn) -> ip::index {
    plain_functions_.emplace(fn.symbol, &fn);
    request_body(fn);
    return constants().intern(fn.symbol, *fn.type);
}

auto compilation::plain_function(ip::index value) -> stdx::option<instance&> {
    return named_function(plain_functions_, value);
}

auto compilation::mangle_value(ip::index value) -> std::string {
    // A copy is spelled by what it was copied from
    if (const auto copy{named_function(const_functions_, value)}) {
        return std::string{
            std::string_view{copy->symbol}.substr(std::string_view{"closureconst."}.size())};
    }

    return gir::const_value::from_interned(constants(), value).mangle();
}

auto compilation::declare_comptime_closure(const unit&                        u,
                                           const instance&                    enclosing,
                                           inst_id                            func,
                                           member_context                     member,
                                           std::vector<stdx::option<operand>> env,
                                           u32 frame) -> result<operand> {
    member.symbol = lasting_name(fmt::format("closure{}", local_count_++));
    const auto declared{TRY(declare_function(u, enclosing.decl, func, member))};
    if (const auto fn{declared.as_opt<function_ref>()}) {
        defer_body(*fn->fn);
        fn->fn->comptime_env = std::move(env);
        fn->fn->env_frame    = frame;
    } else if (const auto g{declared.as_opt<generic_ref>()}) {
        g->fn->comptime_env = std::move(env);
        g->fn->env_frame    = frame;
    }

    return declared;
}

auto compilation::closure_body(const gs::type& type) -> stdx::option<instance&> {
    const auto found{closure_bodies_.find(&type)};
    if (found == closure_bodies_.end()) { return stdx::none; }
    return *found->second;
}

auto compilation::closure_generic(const gs::type& type) -> stdx::option<generic_fn&> {
    const auto found{closure_generics_.find(&type)};
    if (found == closure_generics_.end()) { return stdx::none; }
    return *found->second;
}

auto analyzer::wrapping_closure(inst_id func) const -> bool {
    // Only the loads and captures of what the closure captures come between the two
    const auto& m{mod()};
    for (auto at{std::to_underlying(func) + 1}; at < m.instruction_count(); ++at) {
        const inst_id next{at};
        const auto    t{m.tag_of(next)};
        if (t == tag::closure) {
            record_reader rec{m, m.data_of(next).pl.extra};
            return as_inst(*rec.ref_field()) == func;
        }

        if (t != tag::load && t != tag::capture) { return false; }
    }

    return false;
}

auto analyzer::binding_name_of(inst_id value) const -> std::string_view {
    // A binding is numbered before its initializer, so any instruction may be the one
    const auto& m{mod()};
    for (u32 at{0}; at < m.instruction_count(); ++at) {
        const inst_id next{at};
        const auto    t{m.tag_of(next)};
        if (t != tag::let_value && t != tag::const_local) { continue; }
        record_reader rec{m, m.data_of(next).pl.extra};
        const auto    name{rec.str_field()};
        DISCARD(rec.ref_field());
        if (t == tag::let_value) {
            if (as_inst(*rec.ref_field()) == value) { return name; }
            continue;
        }

        const auto body{rec.body_field()};
        if (!body.empty() && m.tag_of(body.back()) == tag::yield &&
            as_inst(m.data_of(body.back()).un.operand) == value) {
            return name;
        }
    }

    return "anonymous";
}

auto analyzer::local_generic_signature(inst_id func, gsl::span<const ref> captured)
    -> result<gsl::not_null<gs::type*>> {
    // A type the enclosing body knows is that type, one a call binds stays `auto`
    const auto& m{mod()};
    auto        rec{record(func)};
    const auto  params{params_of(m, rec.body_field())};
    const auto  ret{rec.body_field()};
    auto&       auto_type{builtin(gs::type_kind::AUTO)};
    const auto  written{[&](gsl::span<const inst_id> body) -> result<gsl::not_null<gs::type*>> {
        if (body.empty() || m.tag_of(body.back()) != tag::yield) { return &auto_type; }
        const auto  yielded{m.data_of(body.back()).un.operand};
        const auto& at{src_of(body.back())};
        if (body.size() > 2) { return &auto_type; }
        const auto named{as_inst(yielded)};
        if (!named) { return resolve_type(yielded, at); }

        // A type one of the literal's own parameters names is that parameter's `type`
        if (std::ranges::contains(params, *named)) { return &builtin(gs::type_kind::TYPE); }
        if (m.tag_of(*named) != tag::capture) { return &auto_type; }
        const auto slot{m.data_of(*named).un.aux};
        if (slot >= captured.size()) { return &auto_type; }
        const auto value{comptime_value(TRY(resolve(captured[slot], at)))};
        if (!value || !comp_.constants().is_type(*value)) { return &auto_type; }
        return &comp_.constants().type_at(*value);
    }};

    auto& ctx{comp_.ctx()};
    auto  types{ctx.pool.get_many_unsafe(params.size())};
    for (usize i{0}; i < params.size(); ++i) {
        record_reader p{record(params[i])};
        p.skip_str();
        types[i] = TRY(written(p.body_field()));
    }

    auto& return_type{*TRY(written(ret))};
    return &ctx.get_function(types, return_type, false, ast::calling_convention::C, false);
}

auto analyzer::names_itself(stdx::option<inst_id> from) const -> bool {
    if (!from || values_.contains(*from)) { return false; }
    const auto t{mod().tag_of(*from)};
    return t == tag::let_value || t == tag::const_local;
}

auto analyzer::local_function(inst_id                                             func,
                              std::vector<ip::index>                              captures,
                              stdx::option<u32>                                   self_slot,
                              std::vector<std::pair<u32, std::vector<ip::index>>> known_packs)
    -> result<operand> {
    if (const auto known{comp_.local_function_of(u_, func, &*fn_, captures)}) { return *known; }

    auto member{member_context_here()};
    member.captures = std::move(captures);
    return comp_.declare_local(u_,
                               *fn_,
                               {.inst        = func,
                                .func        = func,
                                .member      = std::move(member),
                                .name        = binding_name_of(func),
                                .self_slot   = self_slot,
                                .known_packs = std::move(known_packs)});
}

auto analyzer::comptime_closure(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    if (comp_.speculating()) { return comp_.needs_runtime(u_, src); }
    record_reader rec{record(inst)};
    const auto    func{*as_inst(*rec.ref_field())};
    const auto    count{rec.count()};
    const bool    moves{is_move_fn(mod(), func)};

    // Every capture is known, or a place of compile-time memory the body borrows
    std::vector<ip::index>             known;
    std::vector<stdx::option<operand>> env;
    bool                               borrows{false};
    const auto                         placeholder{capture_placeholder()};
    for (u32 i{0}; i < count; ++i) {
        const auto value_ref{*rec.ref_field()};
        const bool by_ref{(rec.u32_field() & flags::CAPTURE_BY_REF) != 0};
        rec.skip_str();
        if (names_itself(as_inst(value_ref))) { return comp_.needs_runtime(u_, src); }

        // A `move fn` captures a read of a `let mut`, whose memory it copies
        auto read{value_ref};
        if (const auto from{as_inst(value_ref)};
            moves && from && mod().tag_of(*from) == tag::load) {
            read = mod().data_of(*from).un.operand;
        }

        const auto target{TRY(resolve(read, src))};
        const auto at{target.as_opt<comptime_place>()};
        if (by_ref && !at) { return comp_.needs_runtime(u_, src); }

        // A `move fn` keeps a copy of its own, which its calls write
        if (at && moves) {
            env.emplace_back(operand{comp_.new_alloc({
                .type                = type_of(target),
                .value               = comptime_value(load_comptime(*at)),
                .frame               = frame_,
                .runtime_depth       = runtime_depth_,
                .keeps_comptime_type = false,
            })});
            known.emplace_back(placeholder);
            borrows = true;
            continue;
        }

        if (by_ref) {
            env.emplace_back(target);
            known.emplace_back(placeholder);
            borrows = true;
            continue;
        }

        const auto index{comptime_value(at ? load_comptime(*at) : target)};
        if (!index) { return comp_.needs_runtime(u_, src); }
        env.emplace_back(stdx::none);
        known.emplace_back(*index);
    }

    if (!borrows) { return local_function(func, std::move(known)); }
    auto member{member_context_here()};
    member.captures = std::move(known);
    return comp_.declare_comptime_closure(
        u_, *fn_, func, std::move(member), std::move(env), frame_);
}

auto analyzer::local_closure(inst_id inst) -> result<operand> {
    const auto&   src{src_of(inst)};
    record_reader rec{record(inst)};
    const auto    func{*as_inst(*rec.ref_field())};
    const auto    count{rec.count()};

    // A compile-time call's closure, or one borrowing compile-time memory, runs only there
    if (!gir_fn_ || (is_comptime() && (captures_by_ref(mod(), inst) || is_move_fn(mod(), func)))) {
        return comptime_closure(inst);
    }

    const bool moves{is_move_fn(mod(), func)};

    auto&                                               ctx{comp_.ctx()};
    std::vector<ip::index>                              known;
    std::vector<stdx::option<u32>>                      fields;
    std::vector<gs::types::closure_capture>             captures;
    std::vector<gir::value>                             stored;
    stdx::option<u32>                                   self_slot;
    std::vector<named_capture>                          named;
    std::vector<ref>                                    capture_refs;
    std::vector<std::pair<u32, u32>>                    pack_slots;
    std::vector<std::pair<u32, std::vector<ip::index>>> known_packs;
    const auto                                          placeholder{capture_placeholder()};
    for (u32 i{0}; i < count; ++i) {
        const auto value_ref{*rec.ref_field()};
        capture_refs.emplace_back(value_ref);
        const auto bits{rec.u32_field()};
        const bool by_ref{(bits & flags::CAPTURE_BY_REF) != 0};
        const auto name{comp_.lasting_name(rec.str_field())};
        const auto from{as_inst(value_ref)};
        // The literal's own binding, which a generic calls itself through
        if (names_itself(from)) {
            self_slot.emplace(i);
            fields.emplace_back(stdx::none);
            known.emplace_back(placeholder);
            continue;
        }

        // A captured place is borrowed, or copied by a `move fn`
        if (by_ref) {
            const auto target{TRY(resolve(value_ref, src))};
            const auto at{target.as_opt<place>()};
            if (!at || !at->is_mutable || at->pending) {
                return comp_.unsupported(u_, src, "a capture by reference of this place");
            }

            auto&      captured{*at->pointee};
            const auto mode{moves ? gs::types::capture_mode::VALUE
                                  : gs::types::capture_mode::MUT_REF};
            auto& storage{moves ? captured : ctx.get_reference(gs::types::mut::MUTABLE, captured)};
            stored.emplace_back(moves ? load(*at) : gir::value{at->address.data, storage});
            fields.emplace_back(static_cast<u32>(captures.size()));
            captures.emplace_back(gs::types::closure_capture{name, &captured, &storage, mode});
            known.emplace_back(placeholder);
            continue;
        }

        // A `const`, a compile-time parameter, a type or a function cannot be stored
        const auto value{TRY(resolve_value(value_ref, src))};

        // A pack is copied element by element, from the fields that follow its slot
        if (const auto pack{value.as_opt<pack_ref>()}) {
            if (is_comptime()) { return comp_.needs_runtime(u_, src); }
            const auto elements{packs_[pack->index]};

            // A pack only measured stores nothing, its elements known by type alone
            if ((bits & flags::CAPTURE_LEN_ONLY) != 0 && !comp_.is_generic(u_, func)) {
                std::vector<ip::index> typed;
                for (const auto& element : elements) {
                    auto& type{type_of(TRY(concrete(element, src)))};
                    typed.emplace_back(comp_.constants().intern(ip::undefined_value{}, type));
                }

                known_packs.emplace_back(i, std::move(typed));
                fields.emplace_back(stdx::none);
                known.emplace_back(placeholder);
                continue;
            }

            if (elements.empty()) {
                return comp_.unsupported(u_, src, "a capture of an empty pack");
            }
            fields.emplace_back(static_cast<u32>(captures.size()));
            pack_slots.emplace_back(i, static_cast<u32>(elements.size()));
            for (usize k{0}; k < elements.size(); ++k) {
                const auto typed{TRY(concrete(elements[k], src))};
                auto&      captured{type_of(typed)};
                stored.emplace_back(TRY(as_runtime(typed, src)));
                stored.back().type.emplace(captured);
                captures.emplace_back(
                    gs::types::closure_capture{comp_.lasting_name(fmt::format("{}#{}", name, k)),
                                               &captured,
                                               &captured,
                                               gs::types::capture_mode::VALUE});
            }

            known.emplace_back(placeholder);
            continue;
        }

        auto index{comptime_value(value)};
        // A local function is captured as its value, which a call goes through
        if (const auto fn{value.as_opt<function_ref>()}; fn && !index) {
            index = comp_.function_value(*fn->fn);
        }

        const bool is_const{from && (mod().tag_of(*from) == tag::const_local ||
                                     (mod().tag_of(*from) == tag::param && index))};
        const bool runtime{!is_const && !value.is<function_ref>() && !value.is<generic_ref>() &&
                           !value.is<module_ref>() &&
                           !(index && comp_.constants().is_type(*index))};
        if (!runtime) {
            if (!index) { return comp_.unsupported(u_, src, "a capture of this value"); }
            fields.emplace_back(stdx::none);
            known.emplace_back(*index);
            continue;
        }

        // At compile time a known capture is folded into the function's value
        if (is_comptime()) {
            if (!index || moves) { return comp_.needs_runtime(u_, src); }
            named.emplace_back(named_capture{name, *index});
            fields.emplace_back(stdx::none);
            known.emplace_back(*index);
            continue;
        }

        const auto typed{TRY(concrete(value, src))};
        auto&      captured{type_of(typed)};

        // A capturing generic is called through its environment, so it is borrowed
        if (captured.is_generic_value() && !moves) {
            const auto slot{typed.as_opt<slot_value>()};
            if (!slot) { return comp_.unsupported(u_, src, "a capture of this generic"); }
            auto& storage{ctx.get_reference(gs::types::mut::MUTABLE, captured)};
            stored.emplace_back(gir::value{slot->address.data, storage});
            fields.emplace_back(static_cast<u32>(captures.size()));
            captures.emplace_back(gs::types::closure_capture{
                name, &captured, &storage, gs::types::capture_mode::MUT_REF});
            known.emplace_back(placeholder);
            continue;
        }

        const bool copies{moves || captured_by_value(captured)};
        if (copies) {
            stored.emplace_back(TRY(as_runtime(typed, src)));
        } else if (const auto slot{typed.as_opt<slot_value>()}) {
            stored.emplace_back(slot->address);
        } else {
            stored.emplace_back(spill(TRY(as_runtime(typed, src)), captured, false).address);
        }

        auto& storage{copies ? captured : ctx.get_reference(gs::types::mut::CONSTANT, captured)};
        stored.back().type.emplace(storage);
        fields.emplace_back(static_cast<u32>(captures.size()));
        captures.emplace_back(gs::types::closure_capture{name,
                                                         &captured,
                                                         &storage,
                                                         copies ? gs::types::capture_mode::VALUE
                                                                : gs::types::capture_mode::REF});
        known.emplace_back(placeholder);
    }

    if (is_comptime() && !named.empty()) {
        if (self_slot) { return comp_.needs_runtime(u_, src); }
        const auto plain{TRY(local_function(func, std::move(known)))};

        // A generic's instances read the known captures it was declared with
        if (plain.is<generic_ref>()) { return plain; }
        const auto fn{plain.as_opt<function_ref>()};
        if (!fn) { return comp_.needs_runtime(u_, src); }
        return operand{TRY(comp_.comptime_function(*fn->fn, named))};
    }

    if (captures.empty()) {
        return local_function(func, std::move(known), self_slot, std::move(known_packs));
    }
    if (is_comptime()) { return comp_.needs_runtime(u_, src); }

    auto declared{comp_.local_function_of(u_, inst, &*fn_, known)};
    if (!declared) {
        auto member{member_context_here()};
        member.captures = known;
        stdx::option<gs::type&> signature;
        if (comp_.is_generic(u_, func)) {
            signature.emplace(*TRY(local_generic_signature(func, capture_refs)));
        }

        declared.emplace(TRY(comp_.declare_local(u_,
                                                 *fn_,
                                                 {.inst         = inst,
                                                  .func         = func,
                                                  .member       = std::move(member),
                                                  .name         = binding_name_of(inst),
                                                  .env_captures = std::move(captures),
                                                  .fields       = std::move(fields),
                                                  .pack_slots   = std::move(pack_slots),
                                                  .self_slot    = self_slot,
                                                  .known_packs  = std::move(known_packs),
                                                  .signature    = signature})));
    }

    const auto generic{declared->as_opt<generic_ref>()};
    return build_environment(generic ? *generic->fn->env : *declared->as<function_ref>().fn->env,
                             stored);
}

auto analyzer::capture_placeholder() -> ip::index {
    return comp_.constants().intern(ip::undefined_value{}, builtin(gs::type_kind::VOID_));
}

auto analyzer::build_environment(const closure_env& env, gsl::span<const gir::value> stored)
    -> gir::value {
    // The environment is built in a slot of its own and read whole
    auto&      env_type{*env.type};
    const auto slot{builder_.emit_alloca(env_type)};
    for (usize i{0}; i < stored.size(); ++i) {
        auto&      storage{*stored[i].type};
        const auto field{builder_.emit_get_element_ptr(
            gir::value{slot, env_type}, {usize_constant(i)}, storage)};
        builder_.emit_store(gir::value{field, storage}, stored[i]).is_initializer = true;
    }

    return gir::value{builder_.emit_load(gir::value{slot, env_type}, env_type),
                      env.value_type.value_or(env_type)};
}

auto analyzer::bind_environment(const closure_env& env, const gir::value& self) -> void {
    // Every capture is read once up front
    const auto& closure{env.type->get_data().as<gs::types::closure_t>()};
    env_values_.assign(env.fields.size(), stdx::none);
    if (env.self_slot) {
        env_values_[*env.self_slot] = slot_value{
            .address    = gir::value{self.data, *env.type},
            .type       = env.type,
            .is_mutable = true,
        };
    }

    // A pack's elements are read into a pack of the body's own
    for (const auto& [slot, len] : env.pack_slots) {
        std::vector<operand> elements;
        for (u32 k{0}; k < len; ++k) {
            const auto       field{*env.fields[slot] + k};
            auto&            storage{*closure.captures[field].storage_type};
            const auto       address{builder_.emit_get_element_ptr(
                gir::value{self.data, *env.type}, {usize_constant(field)}, storage)};
            const gir::value at{address, storage};
            if (env.moves) {
                elements.emplace_back(
                    slot_value{.address = at, .type = &storage, .is_mutable = true});
            } else {
                elements.emplace_back(gir::value{builder_.emit_load(at, storage), storage});
            }
        }

        env_values_[slot] = pack_ref{static_cast<u32>(packs_.size())};
        packs_.emplace_back(std::move(elements));
    }

    for (usize slot{0}; slot < env.fields.size(); ++slot) {
        const auto field{env.fields[slot]};
        if (!field || std::ranges::contains(env.pack_slots, slot, &std::pair<u32, u32>::first)) {
            continue;
        }

        const auto& capture{closure.captures[*field]};
        auto&       storage{*capture.storage_type};
        auto&       captured{*capture.captured_type};
        const auto  address{builder_.emit_get_element_ptr(
            gir::value{self.data, *env.type}, {usize_constant(*field)}, storage)};

        // A `move fn` owns its copies, so a write to one is still there on the next call
        if (capture.mode == gs::types::capture_mode::VALUE && env.moves) {
            env_values_[slot] = slot_value{
                .address = gir::value{address, storage}, .type = &storage, .is_mutable = true};
            continue;
        }

        const auto loaded{builder_.emit_load(gir::value{address, storage}, storage)};
        if (capture.mode == gs::types::capture_mode::VALUE) {
            env_values_[slot] = gir::value{loaded, storage};
            continue;
        }

        // A borrowed capture reads like an immutable binding, a mutated one is a place
        const gir::value target{loaded, captured};
        if (captured.is_generic_value()) {
            env_values_[slot] =
                slot_value{.address = target, .type = &captured, .is_mutable = true};
        } else if (capture.mode == gs::types::capture_mode::MUT_REF) {
            env_values_[slot] = place{.address = target, .pointee = &captured, .is_mutable = true};
        } else {
            env_values_[slot] = slot_value{.address = target, .type = &captured};
        }
    }
}

auto analyzer::names_function(const function_ref& fn, gs::type& declared) -> bool {
    auto& thin{*fn.fn->type};
    return &declared == &thin || (declared.get_data().is<gs::types::function>() &&
                                  &comp_.ctx().with_erasure(thin, true) == &declared);
}

auto analyzer::builtin_fn_ctx(const source& src) -> result<operand> {
    if (!fn_) { return comp_.unsupported(u_, src, "an `@fnCtx()` outside a function"); }
    if (!env_self_) { return function_ref{&*fn_}; }

    // In a closure it is the closure itself, called through its own environment
    return slot_value{.address = *env_self_, .type = fn_->env->type, .is_mutable = true};
}

auto analyzer::call_thunk(instance& body, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    TRY(require_runtime(src));
    const auto& impl{body.type->get_data().as<gs::types::function>()};
    if (args.size() != impl.params.size()) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", impl.params.size(), args.size()));
    }

    std::vector<gir::value> values;
    for (usize i{0}; i < args.size(); ++i) {
        const auto given{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            given,
            *impl.params[i],
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = body.symbol}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }

    comp_.request_body(body);
    auto&      ret{*body.return_type};
    const auto result{builder_.emit_call(body.symbol, std::move(values), ret)};
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

auto analyzer::closure_address(const operand& op, const source& src) -> result<gir::value> {
    if (const auto slot{op.as_opt<slot_value>()}) { return slot->address; }
    auto& type{type_of(op)};
    return spill(TRY(as_runtime(op, src)), type, false).address;
}

auto analyzer::call_closure(const operand&       callee,
                            instance&            body,
                            gsl::span<const ref> args,
                            const source&        src) -> result<operand> {
    TRY(require_runtime(src));
    if (args.size() != body.params.size()) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Expected {} arguments, found {}", body.params.size(), args.size()));
    }

    const auto& impl{body.type->get_data().as<gs::types::function>()};
    auto        env{TRY(closure_address(callee, src))};
    env.type.emplace(*impl.params[0]);
    std::vector<gir::value> values{env};
    for (usize i{0}; i < args.size(); ++i) {
        const auto given{TRY(resolve_value(args[i], src))};
        const auto coerced{TRY(coerce_to(
            given,
            *body.params[i],
            src,
            {.what = use_site::kind::ARGUMENT, .arg_index = i + 1, .callee = body.symbol}))};
        values.emplace_back(TRY(as_runtime(coerced, src)));
    }

    comp_.request_body(body);
    auto&      ret{*body.return_type};
    const auto result{builder_.emit_call(body.symbol, std::move(values), ret)};
    if (!result) { return void_value(); }
    return gir::value{*result, ret};
}

auto analyzer::comptime_callable(const operand& op, gs::type& target, const source& src)
    -> result<stdx::option<operand>> {
    // A `^fn(...)` holds the function the same way, and is never null
    const auto pointer{target.get_data().as_opt<gs::types::pointer>()};
    auto&      signature{pointer ? pointer->underlying : target};
    if (!signature.get_data().is<gs::types::function>()) { return stdx::none; }
    if (const auto index{comptime_value(op)}; index && comp_.const_function(*index)) {
        return operand{*index};
    }

    // A generic that captures nothing is its instance for the signature's parameters
    if (const auto generic{op.as_opt<generic_ref>()}; generic && !generic->fn->env) {
        auto&              target_fn{*TRY(instance_for_signature(
            *generic->fn, signature.get_data().as<gs::types::function>(), src))};
        const function_ref instance{&target_fn};
        if (!names_function(instance, signature)) { return stdx::none; }
        return operand{comp_.function_value(target_fn)};
    }

    const auto fn{op.as_opt<function_ref>()};
    if (!fn || fn->fn->env || !names_function(*fn, signature)) { return stdx::none; }
    return operand{comp_.function_value(*fn->fn)};
}

auto analyzer::instance_for_signature(generic_fn&                g,
                                      const gs::types::function& signature,
                                      const source& src) -> result<gsl::not_null<instance*>> {
    std::vector<call_arg> args;
    for (const auto param : signature.params) {
        args.emplace_back(call_arg{.value = operand{gir::value{gir::local_id{}, *param}}});
    }

    auto  binding{TRY(bind_generic(g, args, src))};
    auto& target_fn{*TRY(comp_.instance_for(g, std::move(binding)))};
    TRY(comp_.infer_return(target_fn, u_, src));
    return &target_fn;
}

auto analyzer::as_erased_callable(const operand& op, gs::type& target, const source& src)
    -> result<stdx::option<operand>> {
    if (is_comptime()) { return comptime_callable(op, target, src); }

    // A `^fn(...)` is the same two words, but nullable
    const auto pointer{target.get_data().as_opt<gs::types::pointer>()};
    auto&      signature{pointer ? pointer->underlying : target};
    const auto erased{signature.get_data().as_opt<gs::types::function>()};
    if (!erased || !erased->erased) { return stdx::none; }

    auto& ctx{comp_.ctx()};

    // A function item is its code address, called through a trampoline
    if (const auto fn{op.as_opt<function_ref>()}) {
        auto& thin{*fn->fn->type};
        if (&ctx.with_erasure(thin, true) != &signature) { return stdx::none; }
        TRY(require_runtime(src));
        comp_.request_body(*fn->fn);
        const auto code{constant(comp_.constants().intern(fn->fn->symbol, thin), thin)};
        return operand{gir::value{builder_.emit_make_callable(code, stdx::none, target), target}};
    }

    // A generic that captures nothing is its instance for the signature's parameters
    if (const auto generic{op.as_opt<generic_ref>()}; generic && !generic->fn->env) {
        auto& target_fn{*TRY(instance_for_signature(*generic->fn, *erased, src))};
        auto& thin{*target_fn.type};
        if (&ctx.with_erasure(thin, true) != &signature) { return stdx::none; }
        TRY(require_runtime(src));
        comp_.request_body(target_fn);
        const auto code{constant(comp_.constants().intern(target_fn.symbol, thin), thin)};
        return operand{gir::value{builder_.emit_make_callable(code, stdx::none, target), target}};
    }

    // A closure is its environment's address, and its body is the code
    auto&      given{type_of(op)};
    const bool by_reference{given.get_kind() == gs::type_kind::REFERENCE};
    auto&      from{by_reference ? *gs::pointee_of(given) : given};
    auto& opaque_ptr{ctx.get_pointer(gs::types::mut::CONSTANT, builtin(gs::type_kind::OPAQUE))};
    const auto environment{[&] -> result<gir::value> {
        if (by_reference) { return as_runtime(op, src); }
        const auto address{TRY(closure_address(op, src))};
        return gir::value{builder_.emit_address_of(address, opaque_ptr), opaque_ptr};
    }};
    if (const auto body{comp_.closure_body(from)}) {
        const auto& closure{from.get_data().as<gs::types::closure_t>()};
        if (&ctx.with_erasure(closure.signature, true) != &signature) { return stdx::none; }
        TRY(require_runtime(src));
        const auto env{TRY(environment())};
        comp_.request_body(*body);
        return operand{gir::value{builder_.emit_make_callable(env, body->symbol, target), target}};
    }

    // A capturing generic is its instance for the signature, over its environment
    if (const auto g{comp_.closure_generic(from)}; g && g->env) {
        auto&       target_fn{*TRY(instance_for_signature(*g, *erased, src))};
        const auto& impl{target_fn.type->get_data().as<gs::types::function>()};
        auto        params{ctx.pool.get_many_unsafe(impl.params.size() - 1)};
        for (usize i{1}; i < impl.params.size(); ++i) { params[i - 1] = impl.params[i]; }
        auto& visible{ctx.get_function(params, impl.return_type, false, impl.conv, false)};
        if (&ctx.with_erasure(visible, true) != &signature) { return stdx::none; }
        TRY(require_runtime(src));
        const auto env{TRY(environment())};
        comp_.request_body(target_fn);
        return operand{
            gir::value{builder_.emit_make_callable(env, target_fn.symbol, target), target}};
    }

    // A thin function value, such as an `extern fn`, is wrapped the same way
    const auto thin{from.get_data().as_opt<gs::types::function>()};
    if (!thin || thin->erased || &ctx.with_erasure(from, true) != &signature) { return stdx::none; }
    const auto code{TRY(as_runtime(op, src))};
    return operand{gir::value{builder_.emit_make_callable(code, stdx::none, target), target}};
}

} // namespace ghoti::uir::sema
