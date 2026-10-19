#include "driver/cmd/lsp/snapshot.hh"

#include <atomic>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/module/parse_cache.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "driver/cmd/lsp/workspace_symbols.hh"
#include "support/path_utils.hh"
#include "support/string_utils.hh"

namespace ghoti::lsp {

analysis_snapshot::analysis_snapshot(u64                 revision,
                                     mod::overlay_loader files,
                                     std::ostream&       error_stream,
                                     mod::parse_cache&   cache) noexcept
    : revision_{revision}, files_{std::move(files)},
      session_{stdx::make_box<analysis_session>(files_, error_stream, cache)} {}

auto analysis_snapshot::build(analysis_inputs                        inputs,
                              mod::parse_cache&                      cache,
                              std::ostream&                          error_stream,
                              stdx::option<const std::atomic<bool>&> cancel)
    -> stdx::option<stdx::box<analysis_snapshot>> {
    PROFILE_FUNCTION();
    const auto cancelled{[cancel] { return cancel && cancel->load(std::memory_order_relaxed); }};

    auto  snapshot{stdx::make_box<analysis_snapshot>(
        inputs.revision, std::move(inputs.files), error_stream, cache)};
    auto& session{*snapshot->session_};
    if (cancel) { session.get_analyzer().set_cancellation(*cancel); }

    for (const auto& root : inputs.roots) {
        if (cancelled()) { return stdx::none; }
        // A root whose file is gone (deleted, or its drive unmounted) is kept for when it returns
        if (auto res{session.analyze(root)};
            !res && res.error().get_error() != mod::error::PATH_DOES_NOT_EXIST) {
            fmt::println(error_stream,
                         "lsp: failed to reanalyze known root '{}': {}",
                         root.string(),
                         res.error());
        }
    }
    if (cancelled()) { return stdx::none; }

    // Only what this analysis reached is indexed
    for (const auto& [mod_path, mod] : session.get_manager()) {
        snapshot->workspace_index_[mod_path] = module_workspace_symbols(*mod);
        snapshot->diagnostics_.emplace_back(mod_path, to_lsp_diagnostics(*mod));
    }
    cache.retain([&snapshot](const std::filesystem::path& path) {
        return snapshot->workspace_index_.contains(path);
    });
    return snapshot;
}

auto analysis_snapshot::module_of(const std::filesystem::path& path)
    -> stdx::option<mod::module_t&> {
    return session_->get_manager().find_file_module(path);
}

auto analysis_snapshot::text_of(const std::filesystem::path& path) -> stdx::option<std::string> {
    auto text{files_.load(path)};
    if (!text) { return stdx::none; }
    return std::move(*text);
}

auto analysis_snapshot::workspace_symbols(std::string_view query) -> nlohmann::json {
    auto out = nlohmann::json::array();
    for (const auto& [path, entries] : workspace_index_) {
        const auto uri{path_utils::path_to_uri(path)};
        const auto text{text_of(path).value_or(std::string{})};
        for (const auto& entry : entries) {
            if (!query.empty() && !string_utils::contains_ci(entry.name, query)) { continue; }
            out.push_back({
                {"name", entry.name},
                {"kind", std::to_underlying(entry.kind)},
                {
                    "location",
                    {
                        {"uri", uri},
                        {"range", range_of(entry.range, text)},
                    },
                },
            });
        }
    }
    return out;
}

} // namespace ghoti::lsp
