#pragma once

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

// Tracks open editor buffers as overlays and re-runs a full analysis pass on every change
class document_store {
  public:
    using touched_modules = std::vector<std::pair<std::filesystem::path, nlohmann::json>>;
    using query_result    = std::pair<stdx::box<analysis_session>, gsl::not_null<mod::module*>>;
    using workspace_map =
        ankerl::unordered_dense::map<std::filesystem::path, std::vector<workspace_symbol_entry>>;

  public:
    explicit document_store(std::ostream& error_stream) noexcept : error_stream_{error_stream} {}

    // Overlays `text` for `path`, analyzes its module graph, and returns each touched module's
    // path paired with its LSP diagnostics, ready for `textDocument/publishDiagnostics`
    [[nodiscard]] auto update(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // Drops the overlay for `path`, reverting future analysis to its on-disk contents
    auto close(const std::filesystem::path& path) -> void;

    // Current overlay (or on-disk fallback) text for `path`, if it resolves
    [[nodiscard]] auto text_of(const std::filesystem::path& path) -> stdx::option<std::string>;

    // Re-analyzes `path` from its current overlay content for hover/go-to-def queries; the
    // returned session must outlive any use of the module pointer
    [[nodiscard]] auto analyze(const std::filesystem::path& path)
        -> stdx::result<query_result, mod::diagnostic>;

    // Top-level symbols across every module touched by `update()` so far this session
    [[nodiscard]] auto workspace_symbols(std::string_view query) const -> nlohmann::json;

  private:
    mod::overlay_loader loader_;
    std::ostream&       error_stream_;
    workspace_map       workspace_index_;
};

} // namespace ghoti::lsp
