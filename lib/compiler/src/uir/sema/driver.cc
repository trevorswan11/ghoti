#include <string>
#include <utility>

#include <fmt/format.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/uir/sema/sema.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

compilation::compilation(gs::context& ctx, const program& prog, const options& opts)
    : ctx_{ctx}, prog_{prog}, opts_{opts}, gir_{*prog.root, ctx.arena, ctx.pool.intern()},
      ptr_bits_{codegen::target_facts::resolve(ctx.target_opts.triple_str).ptr_bits} {
    // The builtin types are resolved when the prelude is injected
    ctx_.inject_prelude();
    units_.reserve(prog.modules.size());
    for (u32 i{0}; const auto& lowered : prog.modules) {
        units_.push_back({.source = lowered.source, .uir = &lowered.result.uir, .index = i++});
        decls_.emplace_back(lowered.result.uir.decls().size());
    }
}

auto compilation::run() -> outcome {
    PROFILE_FUNCTION();
    analyze_roots();
    if (!stopped_at_) { drain_queue(); }

    outcome out{
        .gir = stdx::none, .diagnostics = gs::diagnostics{}, .stopped_at = std::move(stopped_at_)};
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
            case decl_kind::VALUE:  DISCARD(analyze_decl(u, decl, *d.node)); break;
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

auto compilation::request_body(instance& fn) -> void {
    if (fn.queued) { return; }
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

auto compilation::report(const unit& u, const source& src, gs::error code, std::string message)
    -> stdx::err<stop> {
    gs::diagnostic d{std::move(message), code, span_of(u, src)};
    d.set_origin(*u.source);
    diagnostics_.push_back(std::move(d));
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
