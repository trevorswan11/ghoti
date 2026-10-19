#pragma once

#include <chrono>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/module/parse_cache.hh"
#include "driver/cmd/lsp/snapshot.hh"

namespace ghoti::lsp {

enum class document_sync_kind : i32 {
    FULL        = 1,
    INCREMENTAL = 2,
};

inline constexpr std::chrono::milliseconds DEFAULT_DEBOUNCE_INTERVAL{150};

// Tracks open editor buffers as overlays, every file ever opened or discovered ("known roots"),
// and the newest finished analysis of them.
//
// Every change bumps the revision. Analyses are built elsewhere from `inputs()` and adopted with
// `install`, or built here on the spot by the synchronous helpers
class document_store {
  public:
    using touched_modules = lsp::touched_modules;

  public:
    explicit document_store(std::ostream& error_stream) noexcept : error_stream_{error_stream} {}

    // Overlays `text` for `path`, leaving the analysis stale
    auto edit(const std::filesystem::path& path, std::string text) -> void;

    // Registers every path as a known root with no overlay text, leaving the analysis stale
    auto add_known_roots(std::vector<std::filesystem::path> paths) -> void;

    // Drops the overlay for `path`, reverting future analysis to its on-disk contents. Does NOT
    // remove `path` from the known-roots set
    auto close(const std::filesystem::path& path) -> void;

    // Current overlay (or on-disk fallback) text for `path`, if it resolves
    [[nodiscard]] auto text_of(const std::filesystem::path& path) -> stdx::option<std::string>;

    [[nodiscard]] auto revision() const noexcept -> u64 { return revision_; }

    // A copy of everything an analysis of the current revision reads
    [[nodiscard]] auto inputs() const -> analysis_inputs;

    // Adopts `snapshot` unless a newer one is already installed, queueing its diagnostics
    auto install(stdx::box<analysis_snapshot> snapshot) -> void;

    // The newest installed analysis, which may predate the current revision
    [[nodiscard]] auto snapshot() noexcept -> stdx::option<analysis_snapshot&>;

    // Whether the installed analysis covers the current revision
    [[nodiscard]] auto is_current() const noexcept -> bool;

    // Diagnostics from installed analyses that haven't been published yet
    [[nodiscard]] auto take_pending_diagnostics() -> touched_modules;

    // Analyzes the current revision now if the installed analysis predates it
    auto rebuild_if_dirty() -> void;

    // `edit` followed by an immediate rebuild, returning each reached module's diagnostics
    [[nodiscard]] auto update(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // `add_known_roots` followed by an immediate rebuild
    [[nodiscard]] auto seed_known_roots(std::vector<std::filesystem::path> paths)
        -> touched_modules;

    // The module analyzed for `path` in the current revision
    [[nodiscard]] auto analyze(const std::filesystem::path& path)
        -> stdx::result<gsl::not_null<mod::module_t*>, mod::diagnostic>;

    // The current revision's module graph, spanning every known root
    [[nodiscard]] auto manager() -> const mod::module_manager&;

    // Top-level symbols across the current revision's modules
    [[nodiscard]] auto workspace_symbols(std::string_view query) -> nlohmann::json;

  private:
    // Registers `path` as a known root (normalized, deduplicated); returns whether it was new
    auto register_known_root(const std::filesystem::path& path) -> bool;

    // The current revision's analysis, building it here first if needed
    auto current() -> analysis_snapshot&;

  private:
    std::ostream&                      error_stream_;
    mod::overlay_loader                loader_;
    std::vector<std::filesystem::path> known_roots_;
    u64                                revision_{0};
    // Parses shared by the analyses built on this thread
    mod::parse_cache                           parse_cache_;
    stdx::option<stdx::box<analysis_snapshot>> snapshot_;
    touched_modules                            pending_diagnostics_;
};

} // namespace ghoti::lsp
