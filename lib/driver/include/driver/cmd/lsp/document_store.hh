#pragma once

#include <chrono>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/session.hh"
#include "driver/cmd/lsp/workspace_symbols.hh"

namespace ghoti::lsp {

enum class document_sync_kind : i32 {
    FULL        = 1,
    INCREMENTAL = 2,
};

inline constexpr std::chrono::milliseconds DEFAULT_THROTTLE_INTERVAL{300};

// Tracks open editor buffers as overlays and re-runs a full analysis pass on every change.
//
// Owns one persistent `analysis_session` spanning every file ever opened ("known root") or
// discovered via a workspace scan, rebuilt from scratch on every update
class document_store {
  public:
    using touched_modules = std::vector<std::pair<std::filesystem::path, nlohmann::json>>;
    using workspace_map =
        ankerl::unordered_dense::map<std::filesystem::path, std::vector<workspace_symbol_entry>>;

  public:
    explicit document_store(
        std::ostream&             error_stream,
        std::chrono::milliseconds throttle_interval = DEFAULT_THROTTLE_INTERVAL) noexcept
        : error_stream_{error_stream}, throttle_interval_{throttle_interval},
          session_{stdx::make_box<analysis_session>(loader_, error_stream_)} {}

    // Overlays `text` for `path`, analyzes its module graph, and returns each touched module's
    // path paired with its LSP diagnostics, always rebuilds immediately
    [[nodiscard]] auto update(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // Same as `update`, but skips the rebuild if the last rebuild  happened < `throttle_interval_`
    // ago
    [[nodiscard]] auto update_throttled(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // Registers every path as a known root with no overlay text, then forces an immediate rebuild
    [[nodiscard]] auto seed_known_roots(std::vector<std::filesystem::path> paths)
        -> touched_modules;

    // Drops the overlay for `path`, reverting future analysis to its on-disk contents. Does NOT
    // remove `path` from the known-roots set
    auto close(const std::filesystem::path& path) -> void;

    // Current overlay (or on-disk fallback) text for `path`, if it resolves
    [[nodiscard]] auto text_of(const std::filesystem::path& path) -> stdx::option<std::string>;

    // Analyzes `path` against the persistent module graph for hover/go-to-def/completion queries.
    // Rebuilds first if a prior `update_throttled()` call left the graph stale
    [[nodiscard]] auto analyze(const std::filesystem::path& path)
        -> stdx::result<gsl::not_null<mod::module*>, mod::diagnostic>;

    // The persistent module graph, spanning every known root. Rebuilds first if stale
    [[nodiscard]] auto manager() -> const mod::module_manager&;

    // Top-level symbols across every module touched so far this session. Rebuilds first if stale
    [[nodiscard]] auto workspace_symbols(std::string_view query) -> nlohmann::json;

  private:
    // Registers `path` as a known root (normalized, deduplicated); returns whether it was new
    auto register_known_root(const std::filesystem::path& path) -> bool;

    // Discards `session_` and reanalyzes every known root, refreshing `workspace_index_`
    auto rebuild() -> void;

    // Rebuilds now if `dirty_`, regardless of the throttle interval
    auto rebuild_if_dirty() -> void;

  private:
    mod::overlay_loader                   loader_;
    std::ostream&                         error_stream_;
    std::chrono::milliseconds             throttle_interval_;
    std::chrono::steady_clock::time_point last_rebuild_{};
    bool                                  dirty_{false};
    stdx::box<analysis_session>           session_;
    std::vector<std::filesystem::path>    known_roots_;
    workspace_map                         workspace_index_;
};

} // namespace ghoti::lsp
