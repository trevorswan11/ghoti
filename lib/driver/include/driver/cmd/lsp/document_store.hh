#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/session.hh"

namespace ghoti::lsp {

// Tracks open editor buffers as overlays and re-runs a full analysis pass on every change
class document_store {
  public:
    using touched_modules = std::vector<std::pair<std::filesystem::path, nlohmann::json>>;
    using query_result    = std::pair<stdx::box<analysis_session>, gsl::not_null<mod::module*>>;

  public:
    explicit document_store(std::ostream& error_stream) noexcept : error_stream_{error_stream} {}

    // Overlays `text` for `path`, analyzes its module graph, and returns each touched module's
    // path paired with its LSP diagnostics, ready for `textDocument/publishDiagnostics`
    [[nodiscard]] auto update(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // Drops the overlay for `path`, reverting future analysis to its on-disk contents
    auto close(const std::filesystem::path& path) -> void;

    // Re-analyzes `path` from its current overlay content for hover/go-to-def queries; the
    // returned session must outlive any use of the module pointer
    [[nodiscard]] auto analyze(const std::filesystem::path& path)
        -> stdx::result<query_result, mod::diagnostic>;

  private:
    mod::overlay_loader loader_;
    std::ostream&       error_stream_;
};

} // namespace ghoti::lsp
