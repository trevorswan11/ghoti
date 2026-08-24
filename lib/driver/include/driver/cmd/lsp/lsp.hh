#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>
#include <stdx/result.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/lsp/document_store.hh"

namespace ghoti::cmd {

// A JSON-RPC/LSP server over stdio
//
// https://www.jsonrpc.org/specification
class lsp_server final : public command {
  public:
    using command::command;
    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;

  private:
    // Dispatches one already-parsed message; returns false once `exit` has been handled
    auto handle_message(const nlohmann::json& message, lsp::document_store& store) -> bool;
    auto handle_initialize(const nlohmann::json& message) -> void;
    auto handle_shutdown(const nlohmann::json& message) -> void;
    auto handle_did_open(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_did_change(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_did_close(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_hover(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto handle_definition(const nlohmann::json& message, lsp::document_store& store) -> void;
    auto publish_diagnostics(const std::filesystem::path& path, const nlohmann::json& diagnostics)
        -> void;

  private:
    bool shutdown_received_{false};
};

} // namespace ghoti::cmd
