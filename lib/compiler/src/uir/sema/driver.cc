#include <algorithm>
#include <string>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/builtins.hh"
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
    units_.reserve(prog.modules.size());
    for (u32 i{0}; const auto& lowered : prog.modules) {
        units_.emplace_back<unit>(
            {.source = lowered.source, .uir = &lowered.result.uir, .index = i++});
        decls_.emplace_back(lowered.result.uir.decls().size());
    }
}

auto compilation::run() -> outcome {
    PROFILE_FUNCTION();
    analyze_roots();
    if (!stopped_at_) { drain_queue(); }

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
    // Every non-generic declaration of every module is a root until lazy builds land
    for (const auto& u : units_) {
        for (const auto decl : u.uir->root_namespace().decls) {
            const auto& d{u.uir->decl_at(decl)};
            switch (d.kind) {
            case decl_kind::VALUE:
                // A declaration that is only `@compileError(...)` reports where it is used
                if (!is_deferred_error(u, d)) { DISCARD(analyze_decl(u, decl, *d.node)); }
                break;
            case decl_kind::IMPORT: break;
            case decl_kind::TEST:
                if (ctx_.testing) { DISCARD(unsupported(u, *d.node, "a `test` block")); }
                break;
            case decl_kind::IMPL:     DISCARD(unsupported(u, *d.node, "an `impl` block")); break;
            case decl_kind::COMPTIME: DISCARD(unsupported(u, *d.node, "a `comptime` block")); break;
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
    DISCARD(rec.u32_field());
    return compile_error_message(*u.uir, rec.refs_field());
}

auto compilation::root_values() -> ankerl::unordered_dense::map<std::string, ip::index> {
    ankerl::unordered_dense::map<std::string, ip::index> values;
    for (const auto& u : units_) {
        if (u.source.get() != prog_.root.get()) { continue; }
        for (const auto decl : u.uir->root_namespace().decls) {
            const auto& state{decls_[u.index][std::to_underlying(decl)]};
            if (state.status != decl_status::RESOLVED || !state.value) { continue; }
            if (const auto index{state.value->as_opt<ip::index>()}) {
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

auto compilation::analyze_body(instance& fn) -> result<void> {
    PROFILE_FUNCTION();
    auto& gir_fn{gir_.add_function(fn.symbol, *fn.type, false, false, false, fn.linkage)};
    if (const auto sig{fn.type->get_data().as_opt<gs::types::function>()}) {
        gir_fn.set_calling_conv(sig->conv);
    }
    analyzer a{*this, *fn.owner, fn, gir_fn};
    return a.analyze_function();
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
        stopped_at_.emplace(sema::unsupported{
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
