#pragma once

#include <chrono>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/workspace_scan.hh"

namespace ghoti::cmd {

// A JSON-RPC/LSP server over stdio
//
// https://www.jsonrpc.org/specification
class lsp_server final : public command {
  public:
    explicit lsp_server(
        std::ostream&             error_stream       = std::cerr,
        std::chrono::milliseconds throttle_interval  = lsp::DEFAULT_THROTTLE_INTERVAL,
        std::vector<std::string>  workspace_excludes = lsp::DEFAULT_WORKSPACE_EXCLUDES,
        usize                     workspace_file_cap = lsp::DEFAULT_WORKSPACE_FILE_CAP) noexcept
        : command{error_stream}, throttle_interval_{throttle_interval},
          workspace_excludes_{std::move(workspace_excludes)},
          workspace_file_cap_{workspace_file_cap} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

  private:
    // Dispatches one already-parsed message; returns false once `exit` has been handled
    auto handle_message(const nlohmann::json& message, lsp::document_store& store) -> bool;
    auto handle_initialize(const nlohmann::json& message) -> void;
    auto handle_initialized(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_shutdown(const nlohmann::json& message) -> void;
    auto handle_did_open(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_did_change(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_did_close(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_hover(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_definition(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_document_symbol(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_workspace_symbol(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_completion(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_code_action(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_references(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_rename(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_formatting(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto publish_diagnostics(const std::filesystem::path& path, const nlohmann::json& diagnostics)
        -> void;
    // Publishes whatever `store` accumulated from its most recent dirty-triggered rebuild
    auto publish_pending_diagnostics(lsp::document_store& store) -> void;

  private:
    bool                               shutdown_received_{false};
    std::chrono::milliseconds          throttle_interval_;
    std::vector<std::string>           workspace_excludes_;
    usize                              workspace_file_cap_;
    std::vector<std::filesystem::path> workspace_roots_;
};

} // namespace ghoti::cmd
