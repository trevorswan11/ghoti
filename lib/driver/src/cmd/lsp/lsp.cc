#include "driver/cmd/lsp/lsp.hh"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "driver/platform/win32.hh"
#include "ghoti/config.h"
#include "support/path_utils.hh"

namespace ghoti::cmd {

namespace {

constexpr i32 METHOD_NOT_FOUND{-32'601};

auto make_response(const nlohmann::json& id, nlohmann::json result) -> nlohmann::json {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

auto make_error_response(const nlohmann::json& id, i32 code, std::string_view message)
    -> nlohmann::json {
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

auto make_notification(std::string_view method, nlohmann::json params) -> nlohmann::json {
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

} // namespace

auto lsp_server::execute() -> stdx::result<void, clap::error> {
#if GHOTI_WINDOWS
    win32::set_binary_stdio();
#endif

    lsp::document_store store{error_stream_};
    while (auto message{lsp::read_message(std::cin, error_stream_)}) {
        try {
            if (!handle_message(*message, store)) { break; }
        } catch (const std::exception& ex) {
            fmt::println(error_stream_, "lsp: error handling message: {}", ex.what());
        }
    }

    // Per the spec, `exit` without a prior `shutdown` should report a non-zero exit code
    if (!shutdown_received_) { return stdx::err{clap::error::UNEXPECTED_ERROR}; }
    return {};
}

auto lsp_server::handle_message(const nlohmann::json& message, lsp::document_store& store) -> bool {
    const auto method{message.value("method", std::string{})};
    if (method == "initialize") {
        handle_initialize(message);
    } else if (method == "shutdown") {
        handle_shutdown(message);
    } else if (method == "exit") {
        return false;
    } else if (method == "initialized") {
        // Sent after the initialize response; intentionally a no-op
    } else if (method == "textDocument/didOpen") {
        handle_did_open(message, store);
    } else if (method == "textDocument/didChange") {
        handle_did_change(message, store);
    } else if (method == "textDocument/didClose") {
        handle_did_close(message, store);
    } else if (message.contains("id")) {
        lsp::write_message(
            std::cout, make_error_response(message["id"], METHOD_NOT_FOUND, "method not found"));
    }
    return true;
}

auto lsp_server::handle_initialize(const nlohmann::json& message) -> void {
    const nlohmann::json capabilities{{"textDocumentSync", 1}}; // TextDocumentSyncKind.Full
    const nlohmann::json server_info{{"name", "ghoti"}, {"version", GHOTI_VERSION_STR}};
    lsp::write_message(
        std::cout,
        make_response(message.at("id"),
                      {{"capabilities", capabilities}, {"serverInfo", server_info}}));
}

auto lsp_server::handle_shutdown(const nlohmann::json& message) -> void {
    shutdown_received_ = true;
    lsp::write_message(std::cout, make_response(message.at("id"), nullptr));
}

auto lsp_server::handle_did_open(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& doc{message.at("params").at("textDocument")};
    const auto  path{path_utils::uri_to_path(doc.at("uri").get<std::string>())};
    if (!path) { return; }

    for (auto& [touched_path, diagnostics] :
         store.update(*path, doc.at("text").get<std::string>())) {
        publish_diagnostics(touched_path, diagnostics);
    }
}

auto lsp_server::handle_did_change(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    const auto& changes{params.at("contentChanges")};
    if (!path || changes.empty()) { return; }

    // Full sync guarantees exactly one entry carrying the whole new document text
    auto text{changes.back().at("text").get<std::string>()};
    for (auto& [touched_path, diagnostics] : store.update(*path, std::move(text))) {
        publish_diagnostics(touched_path, diagnostics);
    }
}

auto lsp_server::handle_did_close(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto uri{message.at("params").at("textDocument").at("uri").get<std::string>()};
    if (const auto path{path_utils::uri_to_path(uri)}) { store.close(*path); }
}

auto lsp_server::publish_diagnostics(const std::filesystem::path& path,
                                     const nlohmann::json&        diagnostics) -> void {
    lsp::write_message(
        std::cout,
        make_notification("textDocument/publishDiagnostics",
                          {{"uri", path_utils::path_to_uri(path)}, {"diagnostics", diagnostics}}));
}

} // namespace ghoti::cmd
