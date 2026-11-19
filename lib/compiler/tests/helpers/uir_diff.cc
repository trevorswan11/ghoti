#include "helpers/uir_diff.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/dumper.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/source_loader.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"
#include "helpers/gir_normalize.hh"
#include "support/diagnostic.hh"
#include "support/env.hh"

namespace ghoti::tests::helpers {

namespace {

// Whether an immutable `let` is initialized by a call, which the old emitter folds
[[nodiscard]] auto has_folded_let(gsl::span<const uir::lowered_module> modules) -> bool {
    for (const auto& item : modules) {
        const auto& m{item.result.uir};
        for (u32 i{0}; i < m.instruction_count(); ++i) {
            const uir::inst_id inst{i};
            if (m.tag_of(inst) != uir::tag::let_value) { continue; }
            uir::record_reader rec{m, m.data_of(inst).pl.extra};
            DISCARD(rec.str_field());
            DISCARD(rec.ref_field());
            const auto init{rec.ref_field().and_then(uir::as_inst)};
            if (init && m.tag_of(*init) == uir::tag::call) { return true; }
        }
    }
    return false;
}

// Whether UIR calls a function with only constant arguments that the old dump never calls
[[nodiscard]] auto has_folded_call(std::string_view uir_text, std::string_view legacy_text)
    -> bool {
    for (usize at{uir_text.find("call @")}; at != std::string_view::npos;
         at = uir_text.find("call @", at + 1)) {
        const auto open{uir_text.find('(', at)};
        const auto close{uir_text.find(')', open)};
        if (open == std::string_view::npos || close == std::string_view::npos) { break; }
        const auto args{stdx::string::substr(uir_text, open + 1, close - open - 1)};
        if (args.find('%') != std::string_view::npos ||
            args.find("param.") != std::string_view::npos) {
            continue;
        }
        if (legacy_text.find(stdx::string::substr(uir_text, at, open - at + 1)) ==
            std::string_view::npos) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto dump_with_flags(const gir::module_t& gir_mod) -> std::string {
    std::ostringstream out;
    gir::dumper        d{out};
    d.set_show_flags(true);
    d.dump(gir_mod);
    return out.str();
}

// Every error is one the plan decided UIR reports where the old pipeline didn't
[[nodiscard]] auto only_decided_rejections(const sema::diagnostics& diags) -> bool {
    return std::ranges::all_of(diags, [](const sema::diagnostic& d) {
        return d.get_error() == sema::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE &&
               d.get_message() == "`comptime let mut` assignment depends on a runtime condition";
    });
}

// Some old checks report without marking their module errored
[[nodiscard]] auto has_errors(const sema::diagnostics& diags) -> bool {
    return std::ranges::any_of(diags, [](const sema::diagnostic& d) {
        return d.snapshot().level == diagnostic_level::ERROR;
    });
}

auto configure(sema::analyzer& analyzer, const pipeline_settings& settings) -> void {
    analyzer.set_testing(settings.testing);
    analyzer.set_optimize_mode(settings.build_mode);
}

[[nodiscard]] auto make_manager(mod::source_loader& loader, library_list libraries)
    -> stdx::box<mod::module_manager> {
    auto modules{stdx::make_box<mod::module_manager>(loader)};
    for (const auto& [name, path] : libraries) { DISCARD(modules->add_library_module(name, path)); }
    return modules;
}

// The old pipeline's normalized GIR, or none when it rejects the program
[[nodiscard]] auto legacy_gir(mod::source_loader&          loader,
                              const std::filesystem::path& root,
                              library_list                 libraries,
                              const pipeline_settings&     settings) -> stdx::option<std::string> {
    auto               modules{make_manager(loader, libraries)};
    std::ostringstream quiet;
    sema::analyzer     analyzer{
        *modules, quiet, false, settings.target_opts, false, settings.runtime_safety};
    configure(analyzer, settings);
    auto       gir_mod{analyzer.analyze(root)};
    const auto module{modules->try_get_file_module(root)};
    if (!gir_mod || !module || (*module)->is_errored() || (*module)->is_poisoned() ||
        modules->any_errored() || has_errors(analyzer.get_ctx().diags)) {
        return stdx::none;
    }
    return normalize_gir(dump_with_flags(*gir_mod));
}

} // namespace

auto uir_diff_status_name(uir_diff_status status) noexcept -> std::string_view {
    switch (status) {
    case uir_diff_status::MATCH:            return "match";
    case uir_diff_status::UNSUPPORTED:      return "unsupported";
    case uir_diff_status::REJECTED:         return "rejected";
    case uir_diff_status::FOLD_PLACEMENT:   return "fold-placement";
    case uir_diff_status::SEMANTICS_CHANGE: return "semantics-change";
    case uir_diff_status::GIR_DIFF:         return "gir-diff";
    case uir_diff_status::ASTGEN_FAILED:    return "astgen-failed";
    case uir_diff_status::LEGACY_REJECTED:  return "legacy-rejected";
    }
    return "unknown";
}

auto diff_pipelines(mod::source_loader&          loader,
                    const std::filesystem::path& root,
                    library_list                 libraries,
                    const pipeline_settings&     settings) -> uir_diff {
    const auto legacy{legacy_gir(loader, root, libraries, settings)};
    if (!legacy) { return {.status = uir_diff_status::LEGACY_REJECTED}; }

    auto                    modules{make_manager(loader, libraries)};
    const sema::cfg_context cfg{.target_opts = settings.target_opts,
                                .facts =
                                    codegen::target_facts::resolve(settings.target_opts.triple_str),
                                .build_mode     = settings.build_mode,
                                .runtime_safety = settings.runtime_safety,
                                .testing        = settings.testing,
                                .modules        = *modules};
    const auto              root_mod{modules->try_get_file_module(root)};
    if (!root_mod) { return {.status = uir_diff_status::LEGACY_REJECTED}; }
    const auto lowered{uir::lower_reachable(**root_mod, {.cfg = cfg, .modules = *modules})};
    for (const auto& item : lowered) {
        if (!item.result.diagnostics.empty()) {
            return {.status = uir_diff_status::ASTGEN_FAILED,
                    .detail = fmt::format("{}", item.result.diagnostics)};
        }
    }

    std::ostringstream quiet;
    sema::analyzer     analyzer{
        *modules, quiet, false, settings.target_opts, false, settings.runtime_safety};
    configure(analyzer, settings);
    auto out{uir::sema::analyze(analyzer.get_ctx(), {.root = root_mod->get(), .modules = lowered})};
    if (out.stopped_at) {
        return {.status = uir_diff_status::UNSUPPORTED, .detail = out.stopped_at->what};
    }
    if (!out.succeeded()) {
        const auto status{only_decided_rejections(out.diagnostics)
                              ? uir_diff_status::SEMANTICS_CHANGE
                              : uir_diff_status::REJECTED};
        return {.status = status, .detail = fmt::format("{}", out.diagnostics)};
    }

    const auto uir_text{normalize_gir(dump_with_flags(*out.gir))};
    if (uir_text == *legacy) { return {.status = uir_diff_status::MATCH}; }
    if (has_folded_let(lowered) || has_folded_call(uir_text, *legacy)) {
        return {.status = uir_diff_status::FOLD_PLACEMENT};
    }
    return {.status = uir_diff_status::GIR_DIFF,
            .detail = fmt::format("--- old\n{}--- uir\n{}", *legacy, uir_text)};
}

auto run_uir_sema(std::string_view source, const pipeline_settings& settings)
    -> stdx::box<uir_sema_run> {
    constexpr std::string_view ROOT{"test.gh"};
    auto                       run{stdx::make_box<uir_sema_run>()};
    run->loader = stdx::make_box<mod::memory_loader>();
    run->loader->add(ROOT, std::string{source});
    run->modules  = stdx::make_box<mod::module_manager>(*run->loader);
    run->analyzer = stdx::make_box<sema::analyzer>(*run->modules, std::cerr, false);
    const auto root{run->modules->try_get_file_module(ROOT)};
    REQUIRE(root);
    REQUIRE_FALSE((*root)->is_errored());
    configure(*run->analyzer, settings);
    auto& ctx{run->analyzer->get_ctx()};
    ctx.target_opts    = settings.target_opts;
    ctx.runtime_safety = settings.runtime_safety;
    const sema::cfg_context cfg{.target_opts = ctx.target_opts,
                                .facts =
                                    codegen::target_facts::resolve(settings.target_opts.triple_str),
                                .build_mode     = ctx.build_mode,
                                .runtime_safety = ctx.runtime_safety,
                                .testing        = false,
                                .modules        = *run->modules};
    run->lowered = uir::lower_reachable(**root, {.cfg = cfg, .modules = *run->modules});
    for (const auto& item : run->lowered) { REQUIRE(item.result.diagnostics.empty()); }
    run->out.emplace(uir::sema::analyze(run->analyzer->get_ctx(),
                                        {.root = root->get(), .modules = run->lowered}));
    return run;
}

auto uir_gir(std::string_view source) -> std::string {
    const auto  run{run_uir_sema(source)};
    const auto& out{*run->out};
    if (out.stopped_at) { FAIL("UIR Sema stopped at " << out.stopped_at->what); }
    if (!out.succeeded()) { FAIL("UIR Sema reported:\n" << fmt::format("{}", out.diagnostics)); }
    return normalize_gir(dump_with_flags(*out.gir));
}

auto report_uir_diff(const uir_diff& diff) -> void {
    const auto path{get_env("GHOTI_TEST_UIR_DIFF_REPORT")};
    if (!path) { return; }
    std::ofstream out{std::string{*path}, std::ios::app | std::ios::binary};
    std::string   flat{diff.status == uir_diff_status::GIR_DIFF ? "" : diff.detail};
    std::ranges::replace(flat, '\n', ' ');
    fmt::println(out, "{}\t{}", uir_diff_status_name(diff.status), flat);
}

} // namespace ghoti::tests::helpers
