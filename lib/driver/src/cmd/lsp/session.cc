#include "driver/cmd/lsp/session.hh"

#include <filesystem>
#include <ostream>
#include <utility>

#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/module/parse_cache.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"

namespace ghoti::lsp {

analysis_session::analysis_session(mod::overlay_loader&            loader,
                                   std::ostream&                   error_stream,
                                   stdx::option<mod::parse_cache&> cache,
                                   bool                            sema_uir) noexcept
    : manager_{loader}, analyzer_{manager_, error_stream, false, {}, true}, sema_uir_{sema_uir} {
    PROFILE_FUNCTION();
    if (cache) { manager_.set_parse_cache(*cache); }
    // The editor checks test blocks too
    analyzer_.set_testing(true);
    const auto stdlib_path{mod::find_stdlib()};
    if (!stdlib_path) {
        fmt::println(error_stream, "could not locate the ghoti standard library");
        return;
    }

    if (auto res{manager_.add_library_module("std", *stdlib_path)}; !res) {
        fmt::println(error_stream, "failed to register stdlib: {}", res.error());
    }
}

auto analysis_session::analyze(const std::filesystem::path& entry_path)
    -> stdx::result<gsl::not_null<mod::module_t*>, mod::diagnostic> {
    PROFILE_FUNCTION();
    // A poisoned/errored module is still returned so its diagnostics can be reported
    if (!sema_uir_) {
        DISCARD(analyzer_.analyze(entry_path));
        return manager_.try_get_file_module(entry_path);
    }

    const auto root{TRY(manager_.try_get_file_module(entry_path))};
    analyze_uir(*root);
    return root;
}

auto analysis_session::analyze_uir(mod::module_t& root) -> void {
    PROFILE_FUNCTION();
    auto&      ctx{analyzer_.get_ctx()};
    const auto cfg{sema::cfg_context::from(ctx)};
    auto lowered{uir::lower_reachable(root, {.cfg = cfg, .modules = manager_, .lenient = true})};

    // AstGen's errors are the module's own, and Sema still runs so the rest keeps its types
    for (auto& item : lowered) {
        if (item.result.diagnostics.empty()) { continue; }
        item.source->absorb_sema_diagnostics(std::move(item.result.diagnostics));
    }

    // Names are indexed once Sema typed what they declare, so each use reads that type
    const auto index{gsl::finally([&] {
        for (auto& item : lowered) { uir::index_names(item); }
    })};

    auto       out{uir::sema::analyze(
        ctx, {.root = &root, .modules = lowered}, {.check_all = true, .index = true})};
    const auto owner_of{[&](const auto& d) -> mod::module_t& {
        return d.origin()
            .and_then([&](const mod::module_t& m) { return manager_.find_file_module(m.path); })
            .value_or(root);
    }};
    for (auto& w : out.warnings) { owner_of(w).warnings.emplace_back(std::move(w)); }
    for (auto& d : out.diagnostics) {
        sema::diagnostics single{};
        single.push_back(d);
        owner_of(d).absorb_sema_diagnostics(std::move(single));
    }
}

} // namespace ghoti::lsp
