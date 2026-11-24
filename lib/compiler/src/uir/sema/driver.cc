#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::sema {

namespace {

// How deep compile-time calls may nest until `@setEvalRecursionLimit` changes it
constexpr usize DEFAULT_RECURSION_LIMIT{256};

} // namespace

compilation::compilation(gs::context& ctx, const program& prog, const options& opts)
    : ctx_{ctx}, prog_{prog}, opts_{opts}, gir_{*prog.root, ctx.arena, ctx.pool.intern()},
      ptr_bits_{codegen::target_facts::resolve(ctx.target_opts.triple_str).ptr_bits},
      limits_{.recursion = DEFAULT_RECURSION_LIMIT, .unroll = ctx.eval_unroll_limit} {
    // The builtin types are resolved when the prelude is injected
    ctx_.inject_prelude();

    // Units are never moved once made, since nominals and instances point at them
    units_.reserve(prog.modules.size() + 1);
    for (u32 i{0}; const auto& lowered : prog.modules) {
        units_.emplace_back<unit>(
            {.source = lowered.source, .uir = &lowered.result.uir, .index = i++});
        decls_.emplace_back(lowered.result.uir.decls().size());
    }

    if (!ctx_.modules.has_builtin_module()) { return; }
    auto&      builtin_mod{ctx_.modules.builtin_module()};
    const auto cfg{gs::cfg_context::from(ctx_)};
    prelude_.emplace(lower_module(builtin_mod, {.cfg = cfg, .modules = ctx_.modules}));
    if (!prelude_->diagnostics.empty()) { return; }
    prelude_unit_.emplace(static_cast<u32>(units_.size()));
    units_.emplace_back<unit>(
        {.source = &builtin_mod, .uir = &prelude_->uir, .index = *prelude_unit_});
    decls_.emplace_back(prelude_->uir.decls().size());
}

auto compilation::prelude_decl(std::string_view name, const unit& user, const source& use)
    -> result<operand> {
    if (!prelude_unit_) { return unsupported(user, use, "a program without the `builtin` module"); }
    const auto& u{units_[*prelude_unit_]};
    for (const auto decl : u.uir->root_namespace().decls) {
        if (u.uir->string(u.uir->decl_at(decl).name) == name) {
            return analyze_decl(u, decl, *u.uir->decl_at(decl).node);
        }
    }

    return unsupported(user, use, fmt::format("the `builtin` module's `{}`", name));
}

auto compilation::prelude_function(std::string_view name, const unit& user, const source& use)
    -> result<gsl::not_null<instance*>> {
    // A program's own module-level function of the name overrides the `weak` default
    auto& names{*TRY(declared_names(user, use))};
    if (const auto found{names.candidates.find(std::string{name})};
        found != names.candidates.end()) {
        for (const auto& candidate : found->second) {
            if (prelude_unit_ && candidate.unit == *prelude_unit_) { continue; }
            const auto& u{units_[candidate.unit]};
            const auto& d{u.uir->decl_at(candidate.decl)};
            if (d.has(decl_flags::WEAK) || d.owner) { continue; }
            const auto value{TRY(analyze_decl(u, candidate.decl, *d.node))};
            if (const auto fn{value.as_opt<function_ref>()}) {
                request_body(*fn->fn);
                return fn->fn;
            }
        }
    }

    const auto value{TRY(prelude_decl(name, user, use))};
    const auto fn{value.as_opt<function_ref>()};
    if (!fn) { return unsupported(user, use, fmt::format("the `builtin` module's `{}`", name)); }
    request_body(*fn->fn);
    return fn->fn;
}

auto compilation::prelude_type(std::string_view name, const unit& user, const source& use)
    -> result<gsl::not_null<gs::type*>> {
    const auto value{TRY(prelude_decl(name, user, use))};
    const auto index{value.as_opt<ip::index>()};
    if (!index || !constants().is_type(*index)) {
        return unsupported(user, use, fmt::format("the `builtin` module's `{}`", name));
    }

    auto& type{constants().type_at(*index)};
    TRY(require_fields(user, use, type));
    return &type;
}

auto compilation::run() -> outcome {
    PROFILE_FUNCTION();
    analyze_roots();
    if (!stopped_at_) { drain_queue(); }
    apply_exports();

    outcome out{
        .gir         = stdx::none,
        .diagnostics = gs::diagnostics{},
        .stopped_at  = std::move(stopped_at_),
        .root_values = root_values(),
    };
    if (out.stopped_at) { return out; }
    if (!diagnostics_.empty()) {
        out.diagnostics = std::move(diagnostics_);
        return out;
    }

    out.gir.emplace(std::move(gir_));
    return out;
}

auto compilation::analyze_roots() -> void {
    // Every impl is known before anything looks a member up
    for (const auto& u : units_) {
        for (const auto decl : u.uir->root_namespace().decls) {
            if (u.uir->decl_at(decl).kind == decl_kind::IMPL) { register_impl(u, decl); }
        }
    }

    // TODO: Every non-generic declaration of every module is a root until lazy builds land
    const auto tested{ctx_.testing ? test_units() : std::vector<bool>(units_.size(), false)};
    for (const auto& u : units_) {
        if (prelude_unit_ && u.index == *prelude_unit_) { continue; }
        for (const auto decl : u.uir->root_namespace().decls) {
            const auto& d{u.uir->decl_at(decl)};
            switch (d.kind) {
            case decl_kind::VALUE: {
                // A declaration that is only `@compileError(...)` reports where it is used
                if (is_deferred_error(u, d)) { break; }
                const auto value{analyze_decl(u, decl, *d.node)};
                if (!value) { break; }
                if (const auto type{declared_type(u, d.value_body, *value)}) {
                    analyze_members(u, *type);
                }

                break;
            }
            case decl_kind::IMPORT: break;
            case decl_kind::TEST:
                if (ctx_.testing && tested[u.index]) { analyze_test(u, decl); }
                break;
            case decl_kind::IMPL:     analyze_impl_roots(u, decl); break;
            case decl_kind::COMPTIME: {
                // A top-level statement is checked but never run
                const auto body{u.uir->body(d.value_body)};
                analyzer   block{*this, u};
                if (!u.source->ast.get_as_opt<ast::block_stmt>(*d.node)) {
                    DISCARD(block.analyze_unemitted(body));
                    break;
                }

                DISCARD(block.eval_comptime(body));
                break;
            }
            }

            if (stopped_at_) { return; }
        }
    }
}

auto compilation::drain_queue() -> void {
    PROFILE_FUNCTION();
    while (!queue_.empty() && !stopped_at_) {
        const auto fn{queue_.back()};
        queue_.pop_back();
        if (fn->analyzed) { continue; }
        DISCARD(analyze_body(*fn));
    }
}

auto compilation::is_deferred_error(const unit& u, const decl& d) const -> bool {
    if (!d.value_body) { return false; }
    const auto& m{*u.uir};
    const auto  body{m.body(d.value_body)};
    if (body.empty() || m.tag_of(body.back()) != tag::yield) { return false; }
    const auto yielded{as_inst(m.data_of(body.back()).un.operand)};
    if (!yielded || m.tag_of(*yielded) != tag::builtin) { return false; }
    // Only the message's literal may come before the call
    const bool    only_literals{std::ranges::all_of(body.first(body.size() - 1), [&](inst_id i) {
        return i == *yielded || m.tag_of(i) == tag::str;
    })};
    record_reader rec{m, m.data_of(*yielded).pl.extra};
    return only_literals &&
           static_cast<syntax::builtin_t>(rec.u32_field()) == syntax::builtin_t::COMPILE_ERROR;
}

auto compilation::deferred_error_message(const unit& u, const decl& d) const -> std::string {
    const auto    body{u.uir->body(d.value_body)};
    const auto    call{*as_inst(u.uir->data_of(body.back()).un.operand)};
    record_reader rec{*u.uir, u.uir->data_of(call).pl.extra};
    rec.skip_u32();
    return compile_error_message(*u.uir, rec.refs_field());
}

auto compilation::root_values() -> ankerl::unordered_dense::map<std::string, ip::index> {
    ankerl::unordered_dense::map<std::string, ip::index> values;
    for (const auto& u : units_) {
        if (u.source.get() != prog_.root.get()) { continue; }
        for (const auto decl : u.uir->root_namespace().decls) {
            const auto& state{decls_[u.index][std::to_underlying(decl)]};
            if (state.status != decl_status::RESOLVED || !state.value) { continue; }
            const auto index{[&] -> stdx::option<ip::index> {
                if (const auto g{state.value->as_opt<global_const>()}) { return g->value; }
                return state.value->as_opt<ip::index>().transform([](ip::index i) { return i; });
            }()};
            if (index) {
                values.insert_or_assign(std::string{u.uir->string(u.uir->decl_at(decl).name)},
                                        *index);
            }
        }
    }

    return values;
}

auto compilation::request_body(instance& fn) -> void {
    if (fn.queued || fn.comptime_only) { return; }
    fn.queued = true;
    queue_.emplace_back(&fn);
}

auto compilation::infer_return(instance& fn, const unit& caller, const source& use)
    -> result<void> {
    if (!fn.infers_return || fn.return_type->get_kind() != gs::type_kind::AUTO) { return {}; }
    if (fn.analyzed) {
        return unsupported(
            caller, use, "a recursive call of a function whose return type is inferred");
    }

    const auto analyzed{analyze_body(fn)};
    if (!analyzed) { return stdx::err{analyzed.error()}; }
    return {};
}

auto compilation::analyze_body(instance& fn) -> result<void> {
    PROFILE_FUNCTION();
    fn.analyzed = true;
    auto& gir_fn{gir_.add_function(fn.symbol, *fn.type, false, false, false, fn.linkage)};
    if (const auto sig{fn.type->get_data().as_opt<gs::types::function>()}) {
        gir_fn.set_calling_conv(sig->conv);
    }

    gir_fn.set_weak(fn.weak);
    if (const auto& d{fn.owner->uir->decl_at(fn.decl)}; d.link_name.len != 0) {
        gir_fn.set_link_name(std::string{fn.owner->uir->string(d.link_name)});
    }
    if (!fn.source_name.empty()) { gir_fn.set_source_name(fn.source_name); }
    analyzer   a{*this, *fn.owner, fn, gir_fn};
    const auto analyzed{a.analyze_function()};
    if (fn.infers_return) {
        auto span{ctx_.pool.get_many_unsafe(fn.params.size())};
        for (usize i{0}; i < fn.params.size(); ++i) { span[i] = fn.params[i]; }
        fn.type =
            &ctx_.get_function(span, *fn.return_type, false, ast::calling_convention::C, false);
        if (fn.env) { fn.type = &with_environment(*fn.type, *fn.env->type); }
        gir_fn.set_type(*fn.type);
    }

    if (!analyzed) { return stdx::err{analyzed.error()}; }
    return {};
}

auto compilation::report(const unit&                          u,
                         const source&                        src,
                         gs::error                            code,
                         std::string                          message,
                         const stdx::option<diagnostic_note>& note) -> stdx::err<stop> {
    const auto span{span_of(u, src)};

    // A body run at compile time and analyzed for runtime reports its errors once
    const bool repeated{std::ranges::any_of(diagnostics_, [&](const gs::diagnostic& seen) {
        return seen.get_error() == code && seen.get_message() == message &&
               seen.get_span() == span && seen.origin().get() == u.source.get();
    })};
    if (repeated) { return stdx::err{stop::POISON}; }

    gs::diagnostic d{std::move(message), code, span};
    d.set_origin(*u.source);
    if (note) { d.with_note(note->text, note->at); }
    add_call_notes(d);
    diagnostics_.emplace_back(std::move(d));
    return stdx::err{stop::POISON};
}

auto compilation::unsupported(const unit& u, const source& src, std::string what)
    -> stdx::err<stop> {
    if (!stopped_at_) {
        stopped_at_.emplace<sema::unsupported>({
            .what  = std::move(what),
            .path  = u.source->path,
            .where = location_of(u, src),
        });
    }

    return stdx::err{stop::UNSUPPORTED};
}

auto compilation::location_of(const unit& u, const source& src) const -> source_location {
    return src.visit([&](const auto& id) { return u.source->ast.location_of(id); });
}

auto compilation::span_of(const unit& u, const source& src) const -> source_span {
    return src.visit([&](const auto& id) {
        return source_span{u.source->ast.location_of(id), u.source->ast.end_location_of(id)};
    });
}

auto analyze(gs::context& ctx, const program& prog, const options& opts) -> outcome {
    PROFILE_FUNCTION();
    compilation comp{ctx, prog, opts};
    return comp.run();
}

} // namespace ghoti::uir::sema
