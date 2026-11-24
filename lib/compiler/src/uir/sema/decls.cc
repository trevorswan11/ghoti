#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// The declaration flags this phase gives a meaning to
constexpr auto HANDLED_FLAGS{decl_flags::PUBLIC | decl_flags::CONSTANT | decl_flags::LET |
                             decl_flags::MUT | decl_flags::COMPTIME | decl_flags::WEAK};

constexpr std::string_view GLOBAL_CONSTANT{
    "global variable initializer must be a constant expression"};

[[nodiscard]] auto is_comptime_only(const gs::type& return_type) noexcept -> bool {
    return return_type.get_kind() == gs::type_kind::TYPE ||
           gs::is_comptime_numeric(return_type.get_kind());
}

} // namespace

auto compilation::analyze_decl(const unit& u, decl_id decl, const source& use) -> result<operand> {
    // A member of a type declared outside any call reads the type's namespace
    const auto owner{u.uir->decl_at(decl).owner};
    const auto scope{owner ? existing_nominal(u, *owner) : stdx::none};
    return resolve_decl(
        u,
        decl,
        use,
        [&] -> decl_state& { return decls_[u.index][std::to_underlying(decl)]; },
        [&](analyzer& a) {
            if (scope) { a.enter(*scope); }
        });
}

auto compilation::analyze_member(nominal& n, decl_id decl, const source& use) -> result<operand> {
    if (!n.in_call) { return analyze_decl(*n.owner, decl, use); }
    return resolve_decl(
        *n.owner,
        decl,
        use,
        [&] -> decl_state& { return n.members[decl]; },
        [&](analyzer& a) { a.enter(n); });
}

auto compilation::analyze_impl_decl(impl_instance& applied, decl_id decl, const source& use)
    -> result<operand> {
    return resolve_decl(
        *applied.entry->owner,
        decl,
        use,
        [&] -> decl_state& { return applied.members[decl]; },
        [&](analyzer& a) { enter_impl(applied, a); });
}

template <typename State, typename Setup>
auto compilation::resolve_decl(
    const unit& u, decl_id decl, const source& use, State&& state_of, Setup&& setup)
    -> result<operand> {
    const auto& d{u.uir->decl_at(decl)};
    switch (state_of().status) {
    case decl_status::RESOLVED: return *state_of().value;
    case decl_status::FAILED:   return stdx::err{stop::POISON};
    case decl_status::IN_PROGRESS:
        return report(u,
                      use,
                      gs::error::CYCLIC_DEPENDENCY,
                      fmt::format("'{}' is used during its own resolution", u.uir->string(d.name)));
    case decl_status::UNRESOLVED: break;
    }

    if (is_deferred_error(u, d)) {
        state_of().status = decl_status::FAILED;
        return report(u, use, gs::error::COMPILE_ERROR_REACHED, deferred_error_message(u, d));
    }

    state_of().status = decl_status::IN_PROGRESS;
    analyzer a{*this, u, decl};
    setup(a);
    const auto value{a.analyze_decl(decl)};

    // The analysis may have added states, so the entry is looked up again
    auto& state{state_of()};
    if (!value) {
        state.status = decl_status::FAILED;
        return stdx::err{value.error()};
    }

    state.status = decl_status::RESOLVED;
    state.value.emplace(*value);

    // A type declared here is named for the declaration and resolves its fields once the
    // declaration can be named by them
    if (const auto type{declared_type(u, d.value_body, *value)}) {
        name_nominal(*type, u.uir->string(d.name));
        DISCARD(require_fields(u, use, *type));
    }

    return *value;
}

auto compilation::declared_type(const unit&                   u,
                                const stdx::option<body_ref>& body,
                                const operand&                value) -> stdx::option<gs::type&> {
    const auto index{value.as_opt<ip::index>()};
    if (!index || !constants().is_type(*index)) { return stdx::none; }
    auto&      type{constants().type_at(*index)};
    const auto n{nominal_of(type)};
    const auto insts{u.uir->body(body)};
    if (!n || n->owner->index != u.index || insts.empty() ||
        u.uir->tag_of(insts.back()) != tag::yield ||
        as_inst(u.uir->data_of(insts.back()).un.operand) != n->inst) {
        return stdx::none;
    }

    return type;
}

auto compilation::analyze_members(const unit& u, gs::type& owner) -> void {
    const auto n{nominal_of(owner)};
    if (!n || n->in_call) { return; }
    const auto ns{u.uir->namespace_of(n->inst)};
    if (!ns) { return; }
    for (const auto decl : ns->decls) {
        const auto& d{u.uir->decl_at(decl)};
        if (d.kind != decl_kind::VALUE || is_deferred_error(u, d)) { continue; }
        const auto value{analyze_decl(u, decl, *d.node)};
        if (stopped_at_) { return; }
        if (!value) { continue; }
        if (const auto nested{declared_type(u, d.value_body, *value)}) {
            analyze_members(u, *nested);
        }
    }
}

auto compilation::symbol_for(const unit& u, decl_id decl, const source& src, bool function)
    -> result<std::string> {
    const auto& d{u.uir->decl_at(decl)};
    std::string name{u.uir->string(d.name)};

    // A name declared with storage in more than one scope is qualified by the scope declaring it,
    // unless an outside contract fixes it or it is the program's entry point
    const bool fixed{d.has(decl_flags::EXTERN) || d.has(decl_flags::EXPORT) ||
                     d.has(decl_flags::WEAK) || d.link_name.len != 0};
    const bool entry{u.source.get() == prog_.root.get() && !d.owner && name == ctx_.user_main_name};
    naming_.emplace_back(u.index, decl);
    const auto named{gsl::finally([this] { naming_.pop_back(); })};
    auto       symbol{!fixed && !entry && TRY(collides(name, u, src, function))
                          ? fmt::format("m{}.{}", scope_number(u, d.owner), name)
                          : name};

    const auto [it, fresh]{symbols_.try_emplace(symbol, u.index, decl)};
    if (!fresh && it->second != std::pair{u.index, decl}) {
        return unsupported(u, src, "two declarations named by one symbol");
    }

    return symbol;
}

auto compilation::scoped_symbol(const unit&           scope,
                                stdx::option<inst_id> owner,
                                std::string_view      name,
                                const unit&           user,
                                const source&         use) -> result<std::string> {
    const std::string plain{name};
    if (!TRY(collides(plain, user, use, true))) { return plain; }
    return fmt::format("m{}.{}", scope_number(scope, owner), name);
}

auto compilation::scope_number(const unit& u, stdx::option<inst_id> owner) -> u32 {
    const u64 key{
        (static_cast<u64>(u.index) << 32U) |
        owner.transform([](inst_id i) { return u64{std::to_underlying(i)} + 1; }).value_or(0)};
    const auto next{static_cast<u32>(scope_numbers_.size())};
    return scope_numbers_.try_emplace(key, next).first->second;
}

auto compilation::collides(const std::string& name,
                           const unit&        user,
                           const source&      use,
                           bool               function) -> result<bool> {
    auto&      names{*TRY(declared_names(user, use))};
    const auto found{names.candidates.find(name)};
    if (found == names.candidates.end() || names.weak.contains(name)) { return false; }

    // A declaration that may lack storage only counts once its value shows it has some, while one
    // being named has storage since its symbol is asked for
    const auto is_named{[&](const name_candidate& c) {
        return std::ranges::contains(naming_, std::pair{c.unit, c.decl});
    }};
    u32        count{0};
    for (const auto& c : found->second) { count += c.certain || is_named(c) ? 1 : 0; }
    if (count > 1 || found->second.size() < 2) { return count > 1; }
    for (const auto& c : found->second) {
        if (c.certain || is_named(c)) { continue; }
        const auto& u{units_[c.unit]};
        // A `const` still resolving is on the way to what is being named
        auto& state{decls_[c.unit][std::to_underlying(c.decl)]};
        if (state.status == decl_status::IN_PROGRESS) {
            count += function ? 0 : 1;
            continue;
        }

        // A candidate that can't be resolved yet is resolved again at its next use
        const auto before{speculate()};
        const auto probed{analyze_decl(u, c.decl, *u.uir->decl_at(c.decl).node)};
        if (stopped_at_) { return stdx::err{stop::UNSUPPORTED}; }
        auto& probed_state{decls_[c.unit][std::to_underlying(c.decl)]};
        if (!probed) {
            abandon(before);
            probed_state.status = decl_status::UNRESOLVED;
            probed_state.value.reset();
            count += function ? 0 : 1;
            continue;
        }

        // A value is a global wherever it is used, while a function, type or module isn't one
        const auto known{probed->as_opt<ip::index>()};
        const bool names_value{!probed->is<function_ref>() && !probed->is<generic_ref>() &&
                               !probed->is<module_ref>() &&
                               !(known && constants().is_type(*known))};
        count += probed_state.has_storage || names_value ? 1 : 0;
    }

    return count > 1;
}

auto compilation::declared_names(const unit& user, const source& use)
    -> result<gsl::not_null<scoped_names*>> {
    if (declared_names_) { return &declared_names_.value(); }
    auto&      names{declared_names_.emplace()};
    const auto note{[&](const unit& u, decl_id decl, bool certain) {
        names.candidates[std::string{u.uir->string(u.uir->decl_at(decl).name)}].emplace_back(
            name_candidate{.unit = u.index, .decl = decl, .certain = certain});
    }};

    const auto scan{[&](this const auto& self, const unit& u, decl_id decl) -> void {
        // A function or `let mut` always has storage, a `const` only when its value isn't a type
        const auto& d{u.uir->decl_at(decl)};
        if (d.kind != decl_kind::VALUE || !d.value_body) { return; }
        const auto name{u.uir->string(d.name)};
        if (d.has(decl_flags::WEAK)) { names.weak.emplace(name); }
        if (declares_function(u, d) || d.has(decl_flags::MUT)) {
            note(u, decl, true);
            return;
        }

        const auto owner{declared_nominal(u, d)};
        if (!owner) {
            note(u, decl, false);
            return;
        }

        // an aggregate's members are declared in a scope of their own
        if (u.uir->tag_of(*owner) == tag::interface_decl) { return; }
        if (const auto ns{u.uir->namespace_of(*owner)}) {
            for (const auto member : ns->decls) { self(u, member); }
        }
    }};
    for (const auto& u : units_) {
        for (const auto decl : u.uir->root_namespace().decls) { scan(u, decl); }
    }

    // An impl's methods are its interface's, or its own when it implements none
    for (const auto entry : impls_) {
        const auto& u{*entry->owner};
        if (const auto iface{TRY(impl_interface(*entry))}) {
            const auto n{nominal_of(*iface)};
            if (!n) { return unsupported(user, use, "an impl of an interface UIR didn't declare"); }
            for (const auto& method : interface_methods(*n)) {
                names.candidates[std::string{method.name}].emplace_back(
                    name_candidate{.unit = u.index, .decl = entry->decl, .certain = true});
            }

            continue;
        }

        if (const auto ns{u.uir->namespace_of(entry->inst)}) {
            for (const auto member : ns->decls) {
                const auto& d{u.uir->decl_at(member)};
                if (d.kind == decl_kind::VALUE && declares_function(u, d)) {
                    note(u, member, true);
                }
            }
        }
    }

    return &names;
}

auto compilation::declares_function(const unit& u, const decl& d) -> bool {
    const auto body{u.uir->body(d.value_body)};
    if (body.empty() || u.uir->tag_of(body.back()) != tag::yield) { return false; }
    const auto yielded{as_inst(u.uir->data_of(body.back()).un.operand)};
    return yielded &&
           (u.uir->tag_of(*yielded) == tag::func || u.uir->tag_of(*yielded) == tag::closure);
}

auto compilation::declared_nominal(const unit& u, const decl& d) -> stdx::option<inst_id> {
    const auto body{u.uir->body(d.value_body)};
    if (body.empty() || u.uir->tag_of(body.back()) != tag::yield) { return stdx::none; }
    const auto yielded{as_inst(u.uir->data_of(body.back()).un.operand)};
    if (!yielded) { return stdx::none; }
    switch (u.uir->tag_of(*yielded)) {
    case tag::struct_decl:
    case tag::union_decl:
    case tag::enum_decl:
    case tag::interface_decl: return yielded;
    default:                  return stdx::none;
    }
}

auto holds_types(const gs::type& type) -> bool {
    if (type.get_kind() == gs::type_kind::TYPE) { return true; }
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        return holds_types(arr->underlying);
    }

    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        for (const auto field : st->fields) {
            if (field && holds_types(*field)) { return true; }
        }
    }

    return false;
}

auto compilation::linkage_of(const decl& d) noexcept -> gir::linkage {
    if (d.has(decl_flags::EXPORT)) { return gir::linkage::EXPORT; }
    if (d.has(decl_flags::PUBLIC)) { return gir::linkage::PUBLIC; }
    if (d.has(decl_flags::EXTERN)) { return gir::linkage::EXTERN; }
    return gir::linkage::INTERNAL;
}

auto compilation::member_linkage(const unit& u, const decl& d, const member_context& member)
    -> gir::linkage {
    // An impl's methods and an interface's defaults stay internal whatever their visibility
    const bool in_impl{d.owner && u.uir->tag_of(*d.owner) == tag::impl_decl};
    const bool in_instance{member.scope && member.scope->in_call};
    if (in_impl || in_instance || !member.symbol.empty()) { return gir::linkage::INTERNAL; }
    return linkage_of(d);
}

auto compilation::declare_function(const unit&           u,
                                   decl_id               decl,
                                   inst_id               func,
                                   const member_context& member) -> result<operand> {
    PROFILE_FUNCTION();
    const auto&   m{*u.uir};
    const auto&   d{m.decl_at(decl)};
    const auto&   src{m.src_of(func)};
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{rec.body_field()};
    const auto    ret{rec.body_field()};
    rec.skip_body();
    const auto conv{static_cast<ast::calling_convention>(rec.u32_field())};
    const auto fn_flags{rec.u32_field()};
    const auto attrs{rec.body_field()};
    const auto bound_count{rec.count()};

    // A bodyless literal written as a value is the function type it spells
    const bool type_expr{(fn_flags & flags::BODYLESS) != 0 && (fn_flags & flags::TYPE_EXPR) != 0};
    if (type_expr && bound_count == 0 && !is_generic(u, func)) {
        return function_type_of(u, func, member);
    }

    if ((fn_flags & flags::BODYLESS) != 0) {
        return unsupported(u, src, "a function without a body");
    }

    // `@[testing]` only lets the body use the test builtins
    if ((fn_flags & ~(flags::EXPLICIT_CONV | flags::TESTING | flags::MOVE)) != 0) {
        return unsupported(u, src, "a variadic, extern, erased or `impl` function");
    }

    if (bound_count != 0 || is_generic(u, func)) { return declare_generic(u, decl, func, member); }

    analyzer sig{*this, u};
    sig.enter(member);
    std::vector<gsl::not_null<gs::type*>>      param_types;
    stdx::option<ast::type_modifier::modifier> self_modifier;
    for (const auto param : params) {
        record_reader p{m, m.data_of(param).pl.extra};
        p.skip_str();
        const auto  type_body{p.body_field()};
        const auto  default_body{p.body_field()};
        const auto  param_flags{p.u32_field()};
        const auto  modifier{p.u32_field()};
        const auto& param_src{m.src_of(param)};
        if (!default_body.empty()) { return unsupported(u, param_src, "a default argument"); }
        if ((param_flags & (flags::PARAM_COMPTIME | flags::PARAM_PACK)) != 0) {
            return unsupported(u, param_src, "a comptime or pack parameter");
        }

        if ((param_flags & flags::PARAM_SELF) != 0) {
            const auto mod_kind{static_cast<ast::type_modifier::modifier>(modifier)};
            const auto self{self_param_type(member, mod_kind)};
            if (!self) { return unsupported(u, param_src, "this `self` parameter"); }
            self_modifier.emplace(mod_kind);
            param_types.emplace_back(self.get());
            continue;
        }

        if (modifier != 0) { return unsupported(u, param_src, "a parameter modifier"); }
        auto&      type{*TRY(sig.eval_type(type_body, param_src))};
        const bool is_void{type.get_kind() == gs::type_kind::VOID_};
        if ((!gs::is_value_type(type.get_kind()) && !is_void) ||
            gs::is_comptime_numeric(type.get_kind())) {
            return unsupported(u, param_src, "a parameter of a comptime-only or generic type");
        }

        param_types.emplace_back(&type);
    }

    auto&      return_type{*TRY(sig.eval_type(ret, src))};
    const bool comptime_only{is_comptime_only(return_type)};
    const bool infers_return{return_type.get_kind() == gs::type_kind::AUTO};
    const bool no_value{return_type.get_kind() == gs::type_kind::VOID_ ||
                        return_type.get_kind() == gs::type_kind::NORETURN};
    if (!no_value && !comptime_only && !infers_return &&
        (!gs::is_value_type(return_type.get_kind()) ||
         gs::is_comptime_numeric(return_type.get_kind()))) {
        return unsupported(u, src, "a return type that is comptime-only or generic");
    }

    auto span{ctx_.pool.get_many_unsafe(param_types.size())};
    for (usize i{0}; i < param_types.size(); ++i) { span[i] = param_types[i]; }
    auto& fn_type{ctx_.get_function(span, return_type, false, conv, false)};

    // A non-generic function's attributes are known before its body, for `@typeInfo`
    const auto folded{TRY(sig.fold_attributes(
        attrs,
        {
            .target       = ast::attribute_target::FN,
            .returns_void = return_type.get_kind() == gs::type_kind::VOID_,
            .declared_by =
                declares_literal(m, d, func) ? stdx::option<const uir::decl&>{d} : stdx::none,
        }))};

    auto  symbol{TRY(member_symbol(u, decl, func, member))};
    auto& fn{*instances_.emplace_back(arena_.make<instance>(instance{
        .owner         = &units_[u.index],
        .decl          = decl,
        .func          = func,
        .symbol        = std::move(symbol),
        .type          = &fn_type,
        .params        = std::move(param_types),
        .return_type   = &return_type,
        .linkage       = member_linkage(u, d, member),
        .weak          = d.has(decl_flags::WEAK),
        .queued        = false,
        .comptime_only = comptime_only,
        .bindings      = {},
        .infers_return = infers_return,
        .source_name   = {},
        .captures      = member.captures,
        .self_type     = member.self_type,
        .self_modifier = self_modifier,
        .scope         = member.scope,
        .attributes    = folded,
    }))};
    request_body(fn);
    return function_ref{&fn};
}

auto compilation::function_type_of(const unit& u, inst_id func, const member_context& member)
    -> result<operand> {
    const auto&   m{*u.uir};
    const auto&   src{m.src_of(func)};
    record_reader rec{m, m.data_of(func).pl.extra};
    const auto    params{rec.body_field()};
    const auto    ret{rec.body_field()};
    rec.skip_body();
    const auto conv{static_cast<ast::calling_convention>(rec.u32_field())};
    const auto fn_flags{rec.u32_field()};
    analyzer   sig{*this, u};
    sig.enter(member);

    std::vector<gsl::not_null<gs::type*>> param_types;
    for (const auto param : params) {
        record_reader p{m, m.data_of(param).pl.extra};
        p.skip_str();
        const auto type_body{p.body_field()};
        param_types.emplace_back(TRY(sig.eval_type(type_body, m.src_of(param))));
    }

    auto& return_type{*TRY(sig.eval_type(ret, src))};
    auto  span{ctx_.pool.get_many_unsafe(param_types.size())};
    for (usize i{0}; i < param_types.size(); ++i) { span[i] = param_types[i]; }
    const bool thin{(fn_flags & flags::EXTERN_FN) != 0};
    return operand{
        ctx_.get_function(span, return_type, (fn_flags & flags::VARIADIC) != 0, conv, !thin)
            .get_ip_index()};
}

auto compilation::declare_extern(const unit& u, decl_id decl, gs::type& type) -> result<operand> {
    const auto&       m{*u.uir};
    const auto&       d{m.decl_at(decl)};
    const auto&       sig{type.get_data().as<gs::types::function>()};
    const std::string name{m.string(d.name)};
    const auto        target{d.extern_target.len != 0 ? std::string{m.string(d.extern_target)}
                                                      : std::string{"c"}};
    auto&             gir_fn{
        gir_.add_function(name, type, false, false, sig.is_variadic, gir::linkage::EXTERN, target)};
    gir_fn.set_link_name(d.link_name.len != 0 ? std::string{m.string(d.link_name)} : name);
    gir_fn.set_calling_conv(sig.conv);
    gir_fn.set_weak(d.has(decl_flags::WEAK));
    std::vector<gsl::not_null<gs::type*>> params;
    for (usize i{0}; i < sig.params.size(); ++i) {
        DISCARD(gir_fn.add_param(fmt::format("param.{}", i), *sig.params[i]));
        params.emplace_back(sig.params[i]);
    }

    // Its body is the linker's, so nothing is queued for it
    auto& fn{*instances_.emplace_back(arena_.make<instance>(instance{
        .owner         = &units_[u.index],
        .decl          = decl,
        .func          = inst_id{0},
        .symbol        = name,
        .type          = &type,
        .params        = std::move(params),
        .return_type   = &sig.return_type,
        .linkage       = gir::linkage::EXTERN,
        .weak          = d.has(decl_flags::WEAK),
        .queued        = true,
        .comptime_only = false,
        .bindings      = {},
        .infers_return = false,
        .analyzed      = true,
    }))};
    return function_ref{&fn};
}

auto analyzer::comptime_global(const decl& d, const source& src) -> result<operand> {
    stdx::option<gs::type&> declared;
    if (d.type_body) { declared.emplace(*TRY(eval_type(mod().body(d.type_body), src))); }

    const auto scope{comptime_scope()};
    auto       value{TRY(eval_comptime(mod().body(d.value_body)))};
    if (declared) {
        value = TRY(coerce_to(value, *declared, src, {.what = use_site::kind::STORE}));
    }
    const auto known{comptime_value(value)};
    if (!known) {
        return comp_.report_runtime_need(
            u_, src, gs::error::COMPTIME_EVALUATION_FAILED, std::string{GLOBAL_CONSTANT});
    }

    // Any compile-time evaluation of the program may write it
    auto place{comp_.new_alloc({
        .type                = type_of(value),
        .value               = *known,
        .frame               = frame_,
        .runtime_depth       = 0,
        .keeps_comptime_type = true,
        .lasting             = true,
    })};
    place.borrowed = true;
    return place;
}

auto analyzer::analyze_decl(decl_id decl) -> result<operand> {
    PROFILE_FUNCTION();
    const auto& d{mod().decl_at(decl)};
    ASSERT(d.node, "a module-level declaration has a source node");
    const source src{*d.node};
    if (d.kind == decl_kind::IMPORT) {
        return comp_.import_target(u_, *d.import, mod().string(d.name), src);
    }

    // An `extern` function or global is declared with its type only for the linker to find
    if (d.has(decl_flags::EXTERN) && !d.value_body && d.type_body) {
        const auto unhandled{d.flags &
                             ~(HANDLED_FLAGS | decl_flags::EXTERN | decl_flags::THREADLOCAL)};
        auto&      type{*TRY(eval_type(mod().body(*d.type_body), src))};
        if (unhandled != decl_flags::NONE) {
            return comp_.unsupported(u_, src, "an extern declaration with these modifiers");
        }

        if (type.get_kind() != gs::type_kind::FUNCTION) {
            const auto value{TRY(declare_extern_global(decl, type))};
            const auto attrs{TRY(decl_attributes(d, value))};
            const auto symbol{value.as<global_ref>().symbol};
            for (auto g : comp_.gir().get_globals()) {
                if (g->name != symbol) { continue; }
                g->visibility = attrs.visibility;
                g->alignment  = attrs.alignment;
            }
            return value;
        }

        // Its attributes are the declaration's, for the linker and for `@typeInfo`
        const auto value{TRY(comp_.declare_extern(u_, decl, type))};
        auto&      fn{*value.as<function_ref>().fn};
        fn.attributes = TRY(decl_attributes(d, value));
        for (auto gir_fn : comp_.gir().get_functions()) {
            if (gir_fn->get_name() == fn.symbol && gir_fn->get_linkage() == gir::linkage::EXTERN) {
                gir_fn->set_attributes(*fn.attributes);
            }
        }
        return value;
    }

    const bool     comptime_mut{d.has(decl_flags::COMPTIME) && d.has(decl_flags::MUT)};
    constexpr auto STORAGE_FLAGS{HANDLED_FLAGS | decl_flags::EXPORT | decl_flags::THREADLOCAL};
    if ((d.flags & ~STORAGE_FLAGS) != decl_flags::NONE || d.extern_target.len != 0) {
        return comp_.unsupported(u_, src, "an extern or comptime declaration");
    }

    if (!d.value_body) { return comp_.unsupported(u_, src, "a declaration without a value"); }
    if (comptime_mut) { return comptime_global(d, src); }

    stdx::option<gs::type&> declared;
    bool                    infers{false};
    if (d.type_body) {
        auto& type{*TRY(eval_type(mod().body(d.type_body), src))};
        infers = type.get_kind() == gs::type_kind::AUTO;
        if (!infers) { declared.emplace(type); }
    }

    const auto value{eval_comptime(mod().body(d.value_body))};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.report_runtime_need(
            u_, src, gs::error::COMPTIME_EVALUATION_FAILED, std::string{GLOBAL_CONSTANT});
    }

    if (!value) { return stdx::err{value.error()}; }
    const auto attrs{TRY(decl_attributes(d, *value))};

    // A module alias has no storage
    if (value->is<module_ref>()) {
        if (declared) { return comp_.unsupported(u_, src, "a module stored in a typed global"); }
        return *value;
    }

    if (value->is<function_ref>() || value->is<generic_ref>()) {
        // An immutable one names the function, while a `let mut` keeps it in storage
        const auto fn{value->as_opt<function_ref>()};
        const bool is_mut{d.has(decl_flags::MUT)};
        if (!declared || (fn && !is_mut && names_function(*fn, *declared))) { return *value; }

        // A typed `fn` or `^fn` global holds the function as a value
        const auto held{[&] {
            const auto scope{comptime_scope()};
            return as_erased_callable(*value, *declared, src);
        }()};
        const auto erased{TRY(held)};
        if (!erased) {
            return comp_.unsupported(u_, src, "a function declaration with an explicit type");
        }

        // A constant one is folded where it is read, with no storage
        if (!is_mut) { return *erased; }
        return analyze_global(decl, *erased, declared, attrs, src);
    }

    // An `auto` declaration takes its value's type, a number its default one
    if (infers && gs::is_comptime_numeric(type_of(*value).get_kind())) {
        declared.emplace(comp_.ctx().default_concrete(type_of(*value)));
    }

    return analyze_global(decl, *value, declared, attrs, src);
}

auto analyzer::declares_type(const decl& d) const -> bool {
    const auto body{mod().body(d.value_body)};
    if (body.empty() || mod().tag_of(body.back()) != tag::yield) { return false; }
    const auto yielded{as_inst(mod().data_of(body.back()).un.operand)};
    if (!yielded) { return false; }
    switch (mod().tag_of(*yielded)) {
    case tag::struct_decl:
    case tag::union_decl:
    case tag::enum_decl:
    case tag::interface_decl: return true;
    default:                  return false;
    }
}

auto analyzer::analyze_global(decl_id                        decl,
                              const operand&                 value,
                              stdx::option<gs::type&>        declared,
                              const gs::resolved_attributes& attrs,
                              const source&                  src) -> result<operand> {
    const auto& d{mod().decl_at(decl)};
    const auto  constant_index{comptime_value(value)};
    if (!constant_index) {
        if (is_untyped_literal(value)) {
            if (!declared && value.is<cast_ref>()) {
                return analyze_global(decl, TRY(concrete(value, src)), declared, attrs, src);
            }

            if (!declared) {
                return comp_.unsupported(u_, src, "an untyped literal as a global's value");
            }

            return analyze_global(
                decl,
                TRY(coerce_to(value, *declared, src, {.what = use_site::kind::STORE})),
                declared,
                attrs,
                src);
        }

        return comp_.unsupported(u_, src, "a global of this kind of value");
    }

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

        // Only a module-level alias is listed, by its name alone since it claims no symbol, and
        // never one of `type` itself
        const bool module_level{std::ranges::contains(mod().root_namespace().decls, decl)};
        const bool of_type{pool.type_at(*constant_index).get_kind() == gs::type_kind::TYPE};
        if (!declared && !declares_type(d) && module_level && !of_type && !comp_.is_prelude(u_)) {
            comp_.gir().add_type(std::string{mod().string(d.name)}, pool.type_at(*constant_index));
        }

        return value;
    }

    // An `undefined` that isn't given a storable type has nothing to store
    const bool undefined_type{!declared || declared->get_kind() == gs::type_kind::UNDEFINED};
    if (undefined_type && type_of(value).get_kind() == gs::type_kind::UNDEFINED) { return value; }
    const auto stored{
        declared ? TRY(coerce_to(value, *declared, src, {.what = use_site::kind::STORE})) : value};
    const auto  index{*comptime_value(stored)};
    auto&       type{declared ? *declared : type_of(stored)};
    const bool  is_mutable{d.has(decl_flags::MUT)};
    const auto& data{pool.value_at(index).data};

    // An aggregate has no operand form, so the global holds it apart
    const bool is_aggregate{data.is<ip::aggregate_value>() || data.is<ip::union_value>()};

    // One holding types or is a comptime slice only exists at compile time and has no storage
    if (is_aggregate && !is_mutable &&
        (holds_types(type) || type.get_kind() == gs::type_kind::SLICE)) {
        return stored;
    }

    // A ctor instance's constant is folded wherever it is read, an aggregate built at each read
    if (scope_ && scope_->in_call && !is_mutable) { return stored; }
    // An `undefined` written as the initializer leaves the global unset, one it computes is kept
    const auto init_body{mod().body(d.value_body)};
    const bool written_undefined{
        !init_body.empty() && mod().tag_of(init_body.back()) == tag::yield &&
        as_well_known(mod().data_of(init_body.back()).un.operand) == well_known::undefined};
    const bool is_undefined{data.is<ip::undefined_value>() && written_undefined};
    const bool text_array{data.is<std::string>() && type.get_kind() == gs::type_kind::ARRAY};
    const auto address{data.as_opt<ip::address_value>()};
    if (address && !address->pointee.empty()) {
        return comp_.unsupported(
            u_, src, "a global holding the address of a value with no storage");
    }

    const auto init_index{address && address->pointee.empty()
                              ? pool.intern(std::string{address->symbol}, type)
                              : index};
    auto       symbol_name{TRY(global_symbol(decl, src))};
    auto& global{comp_.gir().add_global(std::move(symbol_name),
                                        type,
                                        !is_mutable,
                                        is_undefined || is_aggregate
                                            ? stdx::none
                                            : stdx::option<gir::value>{constant(init_index, type)},
                                        compilation::linkage_of(d))};
    if (is_aggregate) { global.const_init.emplace(index); }
    if (d.link_name.len != 0) { global.link_name = std::string{mod().string(d.link_name)}; }
    global.is_thread_local = d.has(decl_flags::THREADLOCAL);
    global.is_weak         = d.has(decl_flags::WEAK);
    global.visibility      = attrs.visibility;
    global.alignment       = attrs.alignment;
    comp_.note_storage(u_, decl);
    // A string array is read where the global stores it
    if (!is_mutable && !is_aggregate && !text_array) { return stored; }
    const auto symbol{comp_.lasting_name(global.name)};
    if (!is_mutable) { return global_const{.symbol = symbol, .type = &type, .value = index}; }
    return global_ref{symbol, &type};
}

auto analyzer::declare_extern_global(decl_id decl, gs::type& type) -> result<operand> {
    const auto&       d{mod().decl_at(decl)};
    const std::string name{mod().string(d.name)};
    const auto        target{d.extern_target.len != 0 ? std::string{mod().string(d.extern_target)}
                                                      : std::string{"c"}};

    // Modules declaring the same extern global share one declaration
    for (const auto existing : comp_.gir().get_globals()) {
        if (existing->name == name && existing->linkage == gir::linkage::EXTERN) {
            comp_.note_storage(u_, decl);
            return global_ref{comp_.lasting_name(existing->name), &type};
        }
    }

    auto& global{comp_.gir().add_global(
        name, type, !d.has(decl_flags::MUT), stdx::none, gir::linkage::EXTERN, target)};
    global.link_name       = d.link_name.len != 0 ? std::string{mod().string(d.link_name)} : name;
    global.is_thread_local = d.has(decl_flags::THREADLOCAL);
    global.is_weak         = d.has(decl_flags::WEAK);
    comp_.note_storage(u_, decl);

    // Its value is the linker's, so every read loads it
    return global_ref{comp_.lasting_name(global.name), &type};
}

} // namespace ghoti::uir::sema
