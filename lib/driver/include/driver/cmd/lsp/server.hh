#pragma once

#include <chrono>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <stdx/function.hh>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/command.hh"
#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "driver/cmd/lsp/snapshot.hh"
#include "driver/cmd/lsp/workspace_scan.hh"

namespace ghoti::lsp {

// What the server loop waits on: a message from the client, or an analysis the worker finished
using server_event = stdx::variant<inbound_message, stdx::box<analysis_snapshot>>;

} // namespace ghoti::lsp

namespace ghoti::cmd {

enum : i32 {
    PARSE_ERROR       = -32'700,
    INVALID_REQUEST   = -32'600,
    METHOD_NOT_FOUND  = -32'601,
    INTERNAL_ERROR    = -32'603,
    REQUEST_CANCELLED = -32'800,
};

// A JSON-RPC/LSP server over stdio
//
// https://www.jsonrpc.org/specification
class lsp_server final : public command {
  public:
    static constexpr auto KIND{command_kind::LSP_SERVER};

  public:
    explicit lsp_server(
        std::ostream&             error_stream       = std::cerr,
        std::chrono::milliseconds debounce_interval  = lsp::DEFAULT_DEBOUNCE_INTERVAL,
        std::vector<std::string>  workspace_excludes = lsp::DEFAULT_WORKSPACE_EXCLUDES,
        usize                     workspace_file_cap = lsp::DEFAULT_WORKSPACE_FILE_CAP) noexcept
        : command{error_stream}, debounce_interval_{debounce_interval},
          workspace_excludes_{std::move(workspace_excludes)},
          workspace_file_cap_{workspace_file_cap} {}

    [[nodiscard]] auto execute() -> stdx::result<void, clap::error> override;
    [[nodiscard]] auto get_kind() const noexcept -> command_kind override { return KIND; }

  private:
    // Everything one run owns: the event queue, the editor state, and the analysis worker
    struct state;

    // A query waiting on an analysis at least as new as the revision it arrived at
    struct parked_query {
        nlohmann::json message;
        u64            revision;
    };

  private:
    // Handles events in order, answering requests the client cancelled within the same batch
    // without running them. Returns false once `exit` has been handled
    auto handle_batch(std::vector<lsp::server_event> batch, state& server) -> bool;

    // Runs `handler`, answering `message` with an internal error if it throws
    auto guarded(const nlohmann::json& message, const stdx::function<void()>& handler) -> void;

    // Dispatches one already-parsed message; returns false once `exit` has been handled
    auto handle_message(const nlohmann::json& message, state& server) -> bool;
    auto handle_initialize(const nlohmann::json& message) -> void;
    auto handle_initialized(const nlohmann::json& message, state& server) -> void;
    auto handle_shutdown(const nlohmann::json& message) -> void;
    auto handle_did_open(const nlohmann::json& message, state& server) -> void;
    auto handle_did_change(const nlohmann::json& message, state& server) -> void;
    auto handle_did_close(const nlohmann::json& message, state& server) -> void;
    auto handle_cancel_request(const nlohmann::json& message) -> void;
    auto handle_code_action(const nlohmann::json& message) -> void;
    auto handle_formatting(const nlohmann::json& message, lsp::document_store& store) -> void;

    // Answers a query from `snap`, which covers the text the query was asked about
    auto answer_query(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;
    auto handle_hover(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;
    auto handle_definition(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;
    auto handle_document_symbol(const nlohmann::json& message, lsp::analysis_snapshot& snap)
        -> void;
    auto handle_workspace_symbol(const nlohmann::json& message, lsp::analysis_snapshot& snap)
        -> void;
    auto handle_completion(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;
    auto handle_references(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;
    auto handle_rename(const nlohmann::json& message, lsp::analysis_snapshot& snap) -> void;

    // Installs an analysis the worker finished, publishes its diagnostics, then answers every
    // query it is new enough for
    auto handle_finished_analysis(stdx::box<lsp::analysis_snapshot> snapshot, state& server)
        -> void;

    auto publish_diagnostics(const std::filesystem::path& path, const nlohmann::json& diagnostics)
        -> void;
    // Publishes whatever `store` accumulated from the analyses it installed
    auto publish_pending_diagnostics(lsp::document_store& store) -> void;

    // Abandons the stale analysis, then analyzes the new revision now if a query is waiting, or
    // once the client has been quiet for `debounce_interval_`
    auto note_edit(state& server) -> void;

    // Hands the current revision to the worker unless it is already analyzed or underway
    auto start_analysis(state& server) -> void;

  private:
    bool                      shutdown_received_{false};
    std::chrono::milliseconds debounce_interval_;
    // When the debounced analysis is due, if an edit is waiting for one
    stdx::option<std::chrono::steady_clock::time_point> rebuild_due_;
    // The revision the worker is analyzing, unless it was told to stop
    stdx::option<u64>                  analyzing_revision_;
    std::vector<parked_query>          parked_queries_;
    std::vector<std::string>           workspace_excludes_;
    usize                              workspace_file_cap_;
    std::vector<std::filesystem::path> workspace_roots_;
};

} // namespace ghoti::cmd
