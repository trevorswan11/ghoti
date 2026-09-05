#include "driver/cmd/lsp/server.hh"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/id.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/type.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/lsp/code_actions.hh"
#include "driver/cmd/lsp/completion.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/document_symbols.hh"
#include "driver/cmd/lsp/formatting.hh"
#include "driver/cmd/lsp/position_index.hh"
#include "driver/cmd/lsp/references.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "driver/cmd/lsp/text_edit.hh"
#include "driver/cmd/lsp/workspace_scan.hh"
#include "driver/platform/win32.hh"
#include "ghoti/config.h"
#include "support/diagnostic.hh"
#include "support/path_utils.hh"

namespace ghoti::cmd {

namespace {

constexpr i32 METHOD_NOT_FOUND{-32'601};

auto make_response(const nlohmann::json& id, nlohmann::json result) -> nlohmann::json {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
}

auto make_error_response(const nlohmann::json& id, i32 code, std::string_view message)
    -> nlohmann::json {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {
            "error",
            {
                {"code", code},
                {"message", message},
            },
        },
    };
}

auto make_notification(std::string_view method, nlohmann::json params) -> nlohmann::json {
    return {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", std::move(params)},
    };
}

auto position_from(const nlohmann::json& position) -> source_location {
    return {position.at("line").get<usize>(), position.at("character").get<usize>()};
}

auto write_null_id(const nlohmann::json& message) -> void {
    lsp::write_message(std::cout, make_response(message.at("id"), nullptr));
}

// The `///` doc for whatever `id` resolves to (following go-to-definition across modules),
// falling back to an imported module's `//!` doc when `id` is an import alias.
auto doc_comment_for(const mod::module_manager& manager,
                     const mod::module&         entry,
                     ast::node_id               id,
                     stdx::option<sema::type&>  type) -> stdx::option<std::string> {
    const auto lookup{[&](const mod::module& mod, source_span span) -> stdx::option<std::string> {
        if (const auto doc{mod.ast.doc_for(span)}) { return std::string{*doc}; }
        return stdx::none;
    }};

    if (const auto def{entry.get_identifier_definition(id)}) {
        for (const auto& [path, mod] : manager) {
            if (path == def->path) { return lookup(*mod, def->span); }
        }
        return lookup(entry, def->span);
    }

    if (auto own{lookup(entry, {entry.ast.location_of(id), entry.ast.end_location_of(id)})}) {
        return own;
    }
    if (type) {
        if (const auto mod_type{type->get_data().as_opt<sema::types::module>()}) {
            if (const auto md{mod_type->imported.ast.module_doc()}; !md.empty()) {
                return std::string{md};
            }
        }
    }
    return stdx::none;
}

} // namespace

auto lsp_server::execute() -> stdx::result<void, clap::error> {
#if GHOTI_WINDOWS
    win32::set_binary_stdio();
#endif

    lsp::document_store store{error_stream_, throttle_interval_};
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
        handle_initialized(message, store);
    } else if (method == "textDocument/didOpen") {
        handle_did_open(message, store);
    } else if (method == "textDocument/didChange") {
        handle_did_change(message, store);
    } else if (method == "textDocument/didClose") {
        handle_did_close(message, store);
    } else if (method == "textDocument/hover") {
        handle_hover(message, store);
    } else if (method == "textDocument/definition") {
        handle_definition(message, store);
    } else if (method == "textDocument/documentSymbol") {
        handle_document_symbol(message, store);
    } else if (method == "workspace/symbol") {
        handle_workspace_symbol(message, store);
    } else if (method == "textDocument/completion") {
        handle_completion(message, store);
    } else if (method == "textDocument/codeAction") {
        handle_code_action(message, store);
    } else if (method == "textDocument/references") {
        handle_references(message, store);
    } else if (method == "textDocument/rename") {
        handle_rename(message, store);
    } else if (method == "textDocument/formatting") {
        handle_formatting(message, store);
    } else if (message.contains("id")) {
        lsp::write_message(
            std::cout, make_error_response(message["id"], METHOD_NOT_FOUND, "method not found"));
    }
    return true;
}

auto lsp_server::handle_initialize(const nlohmann::json& message) -> void {
    const auto& params{message.value("params", nlohmann::json::object())};

    workspace_roots_.clear();
    if (const auto it{params.find("workspaceFolders")};
        it != params.end() && it->is_array() && !it->empty()) {
        for (const auto& folder : *it) {
            if (!folder.is_object()) { continue; }
            const auto uri_it{folder.find("uri")};
            if (uri_it == folder.end() || !uri_it->is_string()) { continue; }
            if (const auto path{path_utils::uri_to_path(uri_it->get<std::string>())}) {
                workspace_roots_.emplace_back(*path);
            }
        }
    } else if (const auto root_uri_it{params.find("rootUri")};
               root_uri_it != params.end() && root_uri_it->is_string()) {
        if (const auto path{path_utils::uri_to_path(root_uri_it->get<std::string>())}) {
            workspace_roots_.emplace_back(*path);
        }
    }

    const nlohmann::json capabilities{
        {"textDocumentSync", std::to_underlying(lsp::document_sync_kind::INCREMENTAL)},
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"documentSymbolProvider", true},
        {"workspaceSymbolProvider", true},
        {"completionProvider", {{"triggerCharacters", nlohmann::json::array()}}},
        {"codeActionProvider", true},
        {"referencesProvider", true},
        {"renameProvider", true},
        {"documentFormattingProvider", true},
    };
    const nlohmann::json server_info{{"name", "ghoti"}, {"version", GHOTI_VERSION_STR}};
    lsp::write_message(std::cout,
                       make_response(message.at("id"),
                                     {
                                         {"capabilities", capabilities},
                                         {"serverInfo", server_info},
                                     }));
}

auto lsp_server::handle_initialized(const nlohmann::json&, lsp::document_store& store) -> void {
    if (workspace_roots_.empty()) { return; }

    const auto discovered{
        lsp::discover_workspace_files(workspace_roots_, workspace_excludes_, workspace_file_cap_)};
    for (auto& [touched_path, diagnostics] : store.seed_known_roots(discovered)) {
        publish_diagnostics(touched_path, diagnostics);
    }
}

auto lsp_server::handle_shutdown(const nlohmann::json& message) -> void {
    shutdown_received_ = true;
    write_null_id(message);
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

    auto text{lsp::apply_content_changes(store.text_of(*path).value_or(std::string{}), changes)};
    for (const auto& [touched_path, diagnostics] : store.update_throttled(*path, std::move(text))) {
        publish_diagnostics(touched_path, diagnostics);
    }
}

auto lsp_server::handle_did_close(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto uri{message.at("params").at("textDocument").at("uri").get<std::string>()};
    if (const auto path{path_utils::uri_to_path(uri)}) { store.close(*path); }
}

auto lsp_server::handle_hover(const nlohmann::json& message, lsp::document_store& store) -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto target{position_from(params.at("position"))};
    auto       result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    const auto& entry_module{**result};
    const auto  id{lsp::identifier_at(entry_module, target)};
    if (!id) { return write_null_id(message); }

    const auto type{entry_module.get_sema_type_opt(*id)};
    if (!type) { return write_null_id(message); }

    const auto doc{doc_comment_for(store.manager(), entry_module, *id, type)};

    nlohmann::json hover;
    hover["contents"]["kind"] = doc ? "markdown" : "plaintext";
    hover["contents"]["value"] =
        doc ? fmt::format("```ghoti\n{}\n```\n\n---\n\n{}", type->to_string(), *doc)
            : type->to_string();

    lsp::write_message(std::cout, make_response(message.at("id"), std::move(hover)));
}

auto lsp_server::handle_definition(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto target{position_from(params.at("position"))};
    auto       result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    const auto& entry_module{**result};
    const auto  def_loc{lsp::definition_location_at(entry_module, target)};
    if (!def_loc) { return write_null_id(message); }

    lsp::write_message(std::cout,
                       make_response(message.at("id"),
                                     {
                                         {"uri", path_utils::path_to_uri(def_loc->path)},
                                         {"range", lsp::range_of(def_loc->span)},
                                     }));
}

auto lsp_server::handle_document_symbol(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto path{path_utils::uri_to_path(
        message.at("params").at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    auto result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    lsp::write_message(std::cout, make_response(message.at("id"), lsp::document_symbols(**result)));
}

auto lsp_server::handle_workspace_symbol(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto query{message.at("params").value("query", std::string{})};
    auto       symbols = store.workspace_symbols(query);
    publish_pending_diagnostics(store);
    lsp::write_message(std::cout, make_response(message.at("id"), std::move(symbols)));
}

auto lsp_server::handle_completion(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto target{position_from(params.at("position"))};
    auto       result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    lsp::write_message(std::cout,
                       make_response(message.at("id"), lsp::completion_items(**result, target)));
}

auto lsp_server::handle_code_action(const nlohmann::json& message, lsp::document_store&) -> void {
    const auto& params{message.at("params")};
    const auto  uri{params.at("textDocument").at("uri").get<std::string>()};
    const auto& diagnostics{params.at("context").at("diagnostics")};

    lsp::write_message(std::cout,
                       make_response(message.at("id"), lsp::code_actions(uri, diagnostics)));
}

auto lsp_server::handle_references(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto target{position_from(params.at("position"))};
    auto       result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    const auto& entry_module{**result};
    const auto  definition{lsp::definition_location_at(entry_module, target)};
    if (!definition) { return write_null_id(message); }

    const auto include_declaration{
        params.value("context", nlohmann::json::object()).value("includeDeclaration", false)};

    auto locations = nlohmann::json::array();
    if (include_declaration) {
        locations.push_back({{"uri", path_utils::path_to_uri(definition->path)},
                             {"range", lsp::range_of(definition->span)}});
    }
    for (const auto& ref : lsp::find_references(store.manager(), *definition)) {
        locations.push_back({
            {"uri", path_utils::path_to_uri(ref.path)},
            {"range", lsp::range_of(ref.span)},
        });
    }

    lsp::write_message(std::cout, make_response(message.at("id"), locations));
}

auto lsp_server::handle_rename(const nlohmann::json& message, lsp::document_store& store) -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto target{position_from(params.at("position"))};
    auto       result{store.analyze(*path)};
    publish_pending_diagnostics(store);
    if (!result) { return write_null_id(message); }

    const auto& entry_module{**result};
    const auto  definition{lsp::definition_location_at(entry_module, target)};
    if (!definition) { return write_null_id(message); }

    const auto new_name{params.at("newName").get<std::string>()};

    // Grouped by target file, since a rename can now touch more than one module
    ankerl::unordered_dense::map<std::string, nlohmann::json> edits_by_uri;
    const auto add_edit = [&](const located_span& loc) {
        auto& edits = edits_by_uri[path_utils::path_to_uri(loc.path)];
        if (!edits.is_array()) { edits = nlohmann::json::array(); }
        edits.push_back({
            {"range", lsp::range_of(loc.span)},
            {"newText", new_name},
        });
    };

    add_edit(*definition);
    for (const auto& ref : lsp::find_references(store.manager(), *definition)) { add_edit(ref); }

    auto changes = nlohmann::json::object();
    for (auto& [uri, edits] : edits_by_uri) { changes[uri] = std::move(edits); }
    auto workspace_edit       = nlohmann::json::object();
    workspace_edit["changes"] = std::move(changes);

    lsp::write_message(std::cout, make_response(message.at("id"), workspace_edit));
}

auto lsp_server::handle_formatting(const nlohmann::json& message, lsp::document_store& store)
    -> void {
    const auto& params{message.at("params")};
    const auto  path{
        path_utils::uri_to_path(params.at("textDocument").at("uri").get<std::string>())};
    if (!path) { return write_null_id(message); }

    const auto text{store.text_of(*path)};
    if (!text) { return write_null_id(message); }

    lsp::formatting_options opts{};
    if (params.contains("options")) {
        const auto& options{params.at("options")};
        if (options.contains("tabSize") && options.at("tabSize").is_number_integer()) {
            opts.indent_spaces = static_cast<u16>(options.at("tabSize").get<i64>());
        }
    }

    const auto edits = lsp::format(*text, opts);
    lsp::write_message(std::cout, make_response(message.at("id"), edits));
}

auto lsp_server::publish_diagnostics(const std::filesystem::path& path,
                                     const nlohmann::json&        diagnostics) -> void {
    lsp::write_message(std::cout,
                       make_notification("textDocument/publishDiagnostics",
                                         {
                                             {"uri", path_utils::path_to_uri(path)},
                                             {"diagnostics", diagnostics},
                                         }));
}

auto lsp_server::publish_pending_diagnostics(lsp::document_store& store) -> void {
    for (const auto& [touched_path, diagnostics] : store.take_pending_diagnostics()) {
        publish_diagnostics(touched_path, diagnostics);
    }
}

} // namespace ghoti::cmd
