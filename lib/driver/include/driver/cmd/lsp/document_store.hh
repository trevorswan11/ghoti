#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "compiler/module/overlay_loader.hh"

namespace ghoti::lsp {

// Tracks open editor buffers as overlays and re-runs a full analysis pass on every change
class document_store {
  public:
    using touched_modules = std::vector<std::pair<std::filesystem::path, nlohmann::json>>;

  public:
    explicit document_store(std::ostream& error_stream) noexcept : error_stream_{error_stream} {}

    // Overlays `text` for `path`, analyzes its module graph, and returns each touched module's
    // path paired with its LSP diagnostics, ready for `textDocument/publishDiagnostics`
    [[nodiscard]] auto update(const std::filesystem::path& path, std::string text)
        -> touched_modules;

    // Drops the overlay for `path`, reverting future analysis to its on-disk contents
    auto close(const std::filesystem::path& path) -> void;

  private:
    mod::overlay_loader loader_;
    std::ostream&       error_stream_;
};

} // namespace ghoti::lsp
