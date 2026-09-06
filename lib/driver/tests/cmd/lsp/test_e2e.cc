#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <stdx/types.hh>

#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "support/path_utils.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("ghoti lsp answers initialize/didOpen/hover/shutdown over a real child process") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("hoverProvider").get<bool>());
    CHECK(init_resp.at("result").at("capabilities").at("textDocumentSync").get<i32>() ==
          std::to_underlying(lsp::document_sync_kind::INCREMENTAL));

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e.gh"};
    constexpr std::string_view text{"pub const x := 5;\npub const y := x + 1;\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(diag_note.at("method") == "textDocument/publishDiagnostics");
    CHECK(diag_note.at("params").at("diagnostics").empty());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/hover"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", uri}}},
                                   {
                                       "position",
                                       {
                                           {"line", 1},
                                           {"character", 15},
                                       },
                                   },
                               },
                           },
                       });
    const auto hover_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(hover_resp.at("result").at("contents").at("value") == "constexpr_int");

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp hover surfaces a `///` doc comment on an enum variant and its uses") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    CHECK(lsp::read_message(proc.stdout_stream(), std::cerr));
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_field_doc.gh"};
    constexpr std::string_view text{R"(pub const Color := enum : u32 {
    /// The warm one.
    red = 1u32,
    blue = 2u32,
};
pub const main := fn(): i32 {
    return if (Color.red == Color.red) 0 else 1;
};
)"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    CHECK(lsp::read_message(proc.stdout_stream(), std::cerr));

    const auto hover_at = [&](i32 id, i32 line, i32 character) -> std::string {
        lsp::write_message(proc.stdin_stream(),
                           {
                               {"jsonrpc", "2.0"},
                               {"id", id},
                               {"method", "textDocument/hover"},
                               {"params",
                                {{"textDocument", {{"uri", uri}}},
                                 {"position", {{"line", line}, {"character", character}}}}},
                           });
        return UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr))
            .at("result")
            .at("contents")
            .at("value")
            .get<std::string>();
    };

    // On the variant's own declaration ...
    CHECK(hover_at(2, 2, 5).contains("The warm one."));
    // ... and on a `Color.red` use site.
    CHECK(hover_at(3, 6, 21).contains("The warm one."));

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "shutdown"}});
    CHECK(lsp::read_message(proc.stdout_stream(), std::cerr));
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp offers a missing-semicolon quick fix over a real child process") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("codeActionProvider").get<bool>());
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_code_action.gh"};
    constexpr std::string_view text{"pub const x := 5\n"}; // missing trailing ';'
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto  diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& diagnostics{diag_note.at("params").at("diagnostics")};
    REQUIRE(diagnostics.size() == 1);

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/codeAction"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", uri}}},
                                   {"range", diagnostics[0].at("range")},
                                   {
                                       "context",
                                       {
                                           {"diagnostics", diagnostics},
                                       },
                                   },
                               },
                           },
                       });
    const auto  action_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& actions{action_resp.at("result")};
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].at("title") == "Insert missing ';'");
    const auto& edits{actions[0].at("edit").at("changes").at(uri)};
    CHECK(edits[0].at("newText") == ";");

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "shutdown"},
                       });
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});

    const auto exit_code = UNWRAP(proc.close_stdin_and_wait());
    CHECK(exit_code == 0);
}

TEST_CASE("ghoti lsp applies an incremental didChange edit and re-analyzes it") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.contains("result"));
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_incremental.gh"};
    constexpr std::string_view text{"pub const x := 5;\npub const y := x + 1;\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto initial_diag = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(initial_diag.at("params").at("diagnostics").empty());

    // A zero-width range past the current end of the document appends new text there
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didChange"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", uri}, {"version", 2}}},
                                   {
                                       "contentChanges",
                                       {
                                           {
                                               {
                                                   "range",
                                                   {
                                                       {"start", {{"line", 2}, {"character", 0}}},
                                                       {
                                                           "end",
                                                           {
                                                               {"line", 2},
                                                               {"character", 0},
                                                           },
                                                       },
                                                   },
                                               },
                                               {"text", "pub const z := 99;\n"},
                                           },
                                       },
                                   },
                               },
                           },
                       });
    const auto edited_diag = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(edited_diag.at("params").at("diagnostics").empty());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/documentSymbol"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                       },
                                   },
                               },
                           },
                       });
    const auto  symbols_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& symbols{symbols_resp.at("result")};
    CHECK(lsp::has_field(symbols, "name", "z"));

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "shutdown"},
                       });
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});

    const auto exit_code = UNWRAP(proc.close_stdin_and_wait());
    CHECK(exit_code == 0);
}

TEST_CASE("ghoti lsp reports both a syntax error and a sema error in the same file") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.contains("result"));
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_mixed_errors.gh"};
    constexpr std::string_view text{"pub const x := 5;\n"
                                    "pub const y := undeclared_thing;\n"
                                    "pub const broken := 1\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto  diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& diagnostics{diag_note.at("params").at("diagnostics")};
    REQUIRE(diagnostics.size() == 2);
    const auto has_code = [&](std::string_view code) {
        return std::ranges::any_of(diagnostics, [&](const auto& d) {
            return d.at("code").template get<std::string>() == code;
        });
    };
    CHECK(has_code("UNEXPECTED_TOKEN"));      // the missing semicolon
    CHECK(has_code("UNDECLARED_IDENTIFIER")); // the reference to `undeclared_thing`

    // hover on the first `x` still resolves, proving sema actually ran despite the syntax error
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/hover"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", uri}}},
                                   {
                                       "position",
                                       {
                                           {"line", 0},
                                           {"character", 10},
                                       },
                                   },
                               },
                           },
                       });
    const auto hover_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(hover_resp.at("result").at("contents").at("value") == "constexpr_int");

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "shutdown"},
                       });
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp answers workspace/symbol over a real child process") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("workspaceSymbolProvider").get<bool>());
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_workspace_symbol.gh"};
    constexpr std::string_view text{"pub const findable_thing := 1;\npub const other := 2;\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(diag_note.at("params").at("diagnostics").empty());
    // The server normalizes the URI to an absolute form
    const auto canonical_uri = diag_note.at("params").at("uri").get<std::string>();

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "workspace/symbol"},
                           {
                               "params",
                               {
                                   {"query", "findable"},
                               },
                           },
                       });
    const auto  symbol_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& symbols{symbol_resp.at("result")};
    REQUIRE(symbols.size() == 1);
    CHECK(symbols[0].at("name") == "findable_thing");
    CHECK(symbols[0].at("location").at("uri") == canonical_uri);

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp resolves go-to-definition and references across an import") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("definitionProvider").get<bool>());
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    // Both files share a directory so the relative import resolves without touching real disk
    constexpr std::string_view helper_uri{"file:///C:/ghoti_e2e_xmod/helper.gh"};
    constexpr std::string_view main_uri{"file:///C:/ghoti_e2e_xmod/main.gh"};
    constexpr std::string_view helper_text{"pub const value := 42;\n"};
    constexpr std::string_view main_text{"import \"helper.gh\" as helper;\n"
                                         "pub const x := helper::value;\n"};

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", helper_uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", helper_text},
                                       },
                                   },
                               },
                           },
                       });
    const auto helper_diag = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(helper_diag.at("params").at("diagnostics").empty());
    const auto canonical_helper_uri = helper_diag.at("params").at("uri").get<std::string>();

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", main_uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", main_text},
                                       },
                                   },
                               },
                           },
                       });
    // This didOpen touches both main.gh and helper.gh, so two unordered notifications come back
    std::string canonical_main_uri;
    for (i32 i{0}; i < 2; ++i) {
        const auto note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
        CHECK(note.at("params").at("diagnostics").empty());
        const auto uri = note.at("params").at("uri").get<std::string>();
        if (uri != canonical_helper_uri) { canonical_main_uri = uri; }
    }
    REQUIRE_FALSE(canonical_main_uri.empty());

    // Line 1, column 23 lands on `value` in `helper::value`
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/definition"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", main_uri}}},
                                   {
                                       "position",
                                       {
                                           {"line", 1},
                                           {"character", 23},
                                       },
                                   },
                               },
                           },
                       });
    const auto def_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(def_resp.at("result").at("uri") == canonical_helper_uri);
    CHECK(def_resp.at("result").at("range").at("start").at("character") == 10);

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "textDocument/references"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", main_uri}}},
                                   {"position", {{"line", 1}, {"character", 23}}},
                                   {
                                       "context",
                                       {
                                           {"includeDeclaration", false},
                                       },
                                   },
                               },
                           },
                       });
    const auto  refs_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& locations{refs_resp.at("result")};
    REQUIRE(locations.size() == 1);
    CHECK(locations[0].at("uri") == canonical_main_uri);
    CHECK(locations[0].at("range").at("start").at("character") == 23);

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 4},
                           {"method", "textDocument/references"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", helper_uri}}},
                                   {"position", {{"line", 0}, {"character", 10}}},
                                   {
                                       "context",
                                       {
                                           {"includeDeclaration", false},
                                       },
                                   },
                               },
                           },
                       });
    const auto  upstream_refs_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& upstream_locations{upstream_refs_resp.at("result")};
    REQUIRE(upstream_locations.size() == 1);
    CHECK(upstream_locations[0].at("uri") == canonical_main_uri);
    CHECK(upstream_locations[0].at("range").at("start").at("character") == 23);

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didChange"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", main_uri}, {"version", 2}}},
                                   {
                                       "contentChanges",
                                       {
                                           {
                                               {"text",
                                                "import \"helper.gh\" as helper;\n"
                                                "pub const x := helper::value;\n"
                                                "pub const y := helper::value;\n"},
                                           },
                                       },
                                   },
                               },
                           },
                       });
    for (i32 i{0}; i < 2; ++i) {
        const auto note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
        CHECK(note.at("params").at("diagnostics").empty());
    }

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 5},
                           {"method", "textDocument/references"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", helper_uri}}},
                                   {"position", {{"line", 0}, {"character", 10}}},
                                   {
                                       "context",
                                       {
                                           {"includeDeclaration", false},
                                       },
                                   },
                               },
                           },
                       });
    const auto  edited_refs_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& edited_locations{edited_refs_resp.at("result")};
    REQUIRE(edited_locations.size() == 2);

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 6}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp renames a symbol from its upstream importer's usage") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.contains("result"));
    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nlohmann::json::object()}});

    constexpr std::string_view helper_uri{"file:///C:/ghoti_e2e_rename_xmod/helper.gh"};
    constexpr std::string_view main_uri{"file:///C:/ghoti_e2e_rename_xmod/main.gh"};
    constexpr std::string_view helper_text{"pub const value := 42;\n"};
    constexpr std::string_view main_text{"import \"helper.gh\" as helper;\n"
                                         "pub const x := helper::value;\n"};

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", helper_uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", helper_text},
                                       },
                                   },
                               },
                           },
                       });
    const auto helper_diag          = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto canonical_helper_uri = helper_diag.at("params").at("uri").get<std::string>();

    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"},
         {"method", "textDocument/didOpen"},
         {"params",
          {{"textDocument",
            {{"uri", main_uri}, {"languageId", "ghoti"}, {"version", 1}, {"text", main_text}}}}}});
    std::string canonical_main_uri;
    for (i32 i{0}; i < 2; ++i) {
        const auto note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
        const auto uri  = note.at("params").at("uri").get<std::string>();
        if (uri != canonical_helper_uri) { canonical_main_uri = uri; }
    }
    REQUIRE_FALSE(canonical_main_uri.empty());

    // Rename initiated from helper.gh's own declaration must produce an edit for main.gh too
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/rename"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", helper_uri}}},
                                   {"position", {{"line", 0}, {"character", 10}}},
                                   {"newName", "renamed"},
                               },
                           },
                       });
    const auto  rename_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& changes{rename_resp.at("result").at("changes")};
    REQUIRE(changes.contains(canonical_main_uri));
    const auto& main_edits{changes.at(canonical_main_uri)};
    REQUIRE(main_edits.size() == 1);
    CHECK(main_edits[0].at("newText") == "renamed");
    CHECK(main_edits[0].at("range").at("start").at("character") == 23);

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp throttles rapid didChange notifications but stays content-fresh") {
    // A large explicit window so every didChange in the rapid burst below must land inside it
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "5000"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.contains("result"));
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "initialized"},
                           {"params", nlohmann::json::object()},
                       });

    constexpr std::string_view uri{"file:///test_e2e_throttle.gh"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", "pub const x := 1;\n"},
                                       },
                                   },
                               },
                           },
                       });
    const auto initial_diag = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(initial_diag.at("params").at("diagnostics").empty());

    // Five rapid edits, none read in between
    for (i32 i{2}; i <= 6; ++i) {
        const auto text{i == 6 ? std::string{"pub const x := 1;\npub const z := 99;\n"}
                               : fmt::format("pub const x := {};\n", i)};
        lsp::write_message(proc.stdin_stream(),
                           {{"jsonrpc", "2.0"},
                            {"method", "textDocument/didChange"},
                            {"params",
                             {{"textDocument", {{"uri", uri}, {"version", i}}},
                              {"contentChanges", {{{"text", text}}}}}}});
    }

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/documentSymbol"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                       },
                                   },
                               },
                           },
                       });
    const auto catchup_diag = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    REQUIRE(catchup_diag.contains("method"));
    CHECK(catchup_diag.at("method") == "textDocument/publishDiagnostics");
    CHECK(catchup_diag.at("params").at("diagnostics").empty());

    const auto symbols_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    REQUIRE(symbols_resp.contains("id"));
    CHECK(symbols_resp.at("id") == 2);
    CHECK(lsp::has_field(symbols_resp.at("result"), "name", "z"));

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "shutdown"},
                       });
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp discovers workspace files and resolves references without opening them") {
    tempdir dir{"e2e_workspace_scan"};
    dir.write("helper.gh", "pub const value := 42;\n");
    dir.write("main.gh",
              "import \"helper.gh\" as helper;\n"
              "pub const x := helper::value;\n");

    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    const auto workspace_uri{path_utils::path_to_uri(dir.path)};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                                   {
                                       "workspaceFolders",
                                       {
                                           {
                                               {"uri", workspace_uri},
                                               {"name", "e2e_workspace_scan"},
                                           },
                                       },
                                   },
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.contains("result"));

    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nlohmann::json::object()}});
    // The scan+seed publishes diagnostics for both discovered files, in no guaranteed order
    for (i32 i{0}; i < 2; ++i) {
        const auto note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
        CHECK(note.at("params").at("diagnostics").empty());
    }

    // Neither file was ever opened
    const auto helper_uri{
        path_utils::path_to_uri(std::filesystem::weakly_canonical(dir.path / "helper.gh"))};
    const auto main_uri{
        path_utils::path_to_uri(std::filesystem::weakly_canonical(dir.path / "main.gh"))};

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/references"},
                           {
                               "params",
                               {
                                   {"textDocument", {{"uri", helper_uri}}},
                                   {"position", {{"line", 0}, {"character", 10}}},
                                   {
                                       "context",
                                       {
                                           {"includeDeclaration", false},
                                       },
                                   },
                               },
                           },
                       });
    const auto  refs_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& locations{refs_resp.at("result")};
    REQUIRE(locations.size() == 1);
    CHECK(locations[0].at("uri") == main_uri);
    CHECK(locations[0].at("range").at("start").at("character") == 23);
    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp formats an unformatted document over a real child process") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"capabilities", nlohmann::json::object()},
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("documentFormattingProvider").get<bool>());
    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nlohmann::json::object()}});

    constexpr std::string_view uri{"file:///test_e2e_format.gh"};
    constexpr std::string_view text{"pub const x:i32=10;\npub const y:i32=20;\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", text},
                                       },
                                   },
                               },
                           },
                       });
    const auto diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(diag_note.at("params").at("diagnostics").empty());
    const auto canonical_uri = diag_note.at("params").at("uri").get<std::string>();

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/formatting"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", canonical_uri},
                                       },
                                   },
                                   {
                                       "options",
                                       {
                                           {"tabSize", 4},
                                           {"insertSpaces", true},
                                       },
                                   },
                               },
                           },
                       });
    const auto  fmt_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto& edits{fmt_resp.at("result")};
    REQUIRE(edits.is_array());
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].at("newText") == "pub const x: i32 = 10;\npub const y: i32 = 20;\n");
    CHECK(edits[0].at("range").at("start").at("line") == 0);
    CHECK(edits[0].at("range").at("start").at("character") == 0);
    CHECK(edits[0].at("range").at("end").at("line") == 2);
    CHECK(edits[0].at("range").at("end").at("character") == 0);

    // Document with syntax errors should return null
    constexpr std::string_view broken_uri{"file:///test_e2e_format_broken.gh"};
    constexpr std::string_view broken_text{"pub const broken := ;\n"};
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"method", "textDocument/didOpen"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", broken_uri},
                                           {"languageId", "ghoti"},
                                           {"version", 1},
                                           {"text", broken_text},
                                       },
                                   },
                               },
                           },
                       });
    std::string canonical_broken_uri;
    for (i32 i{0}; i < 2; ++i) {
        const auto note     = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
        const auto note_uri = note.at("params").at("uri").get<std::string>();
        if (note_uri != canonical_uri) {
            canonical_broken_uri = note_uri;
            CHECK_FALSE(note.at("params").at("diagnostics").empty());
        }
    }
    REQUIRE_FALSE(canonical_broken_uri.empty());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 3},
                           {"method", "textDocument/formatting"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", canonical_broken_uri},
                                       },
                                   },
                                   {
                                       "options",
                                       {
                                           {"tabSize", 4},
                                           {"insertSpaces", true},
                                       },
                                   },
                               },
                           },
                       });
    const auto broken_fmt_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(broken_fmt_resp.at("result").is_null());

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 4}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

TEST_CASE("ghoti lsp includes `///` doc comments in hover contents") {
    piped_process proc{mock_argv{ghoti_binary_path().string(), "lsp", "--throttle-ms", "0"}};
    REQUIRE(proc.is_running());

    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 1},
                           {"method", "initialize"},
                           {
                               "params",
                               {
                                   {"processId", nullptr},
                                   {"rootUri", nullptr},
                                   {
                                       "capabilities",
                                       nlohmann::json::object(),
                                   },
                               },
                           },
                       });
    const auto init_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(init_resp.at("result").at("capabilities").at("hoverProvider").get<bool>());
    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"}, {"method", "initialized"}, {"params", nlohmann::json::object()}});

    constexpr std::string_view uri{"file:///test_e2e_doc.gh"};
    constexpr std::string_view text{"/// The magic number.\npub const answer := 42;\n"
                                    "pub const echo := answer;\n"};
    lsp::write_message(
        proc.stdin_stream(),
        {{"jsonrpc", "2.0"},
         {"method", "textDocument/didOpen"},
         {"params",
          {{"textDocument",
            {{"uri", uri}, {"languageId", "ghoti"}, {"version", 1}, {"text", text}}}}}});
    const auto diag_note = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(diag_note.at("method") == "textDocument/publishDiagnostics");

    // Hover the use of `answer` on the third line.
    lsp::write_message(proc.stdin_stream(),
                       {
                           {"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "textDocument/hover"},
                           {
                               "params",
                               {
                                   {
                                       "textDocument",
                                       {
                                           {"uri", uri},
                                       },
                                   },
                                   {
                                       "position",
                                       {
                                           {"line", 2},
                                           {"character", 18},
                                       },
                                   },
                               },
                           },
                       });
    const auto hover_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    const auto value      = hover_resp.at("result").at("contents").at("value").get<std::string>();
    CHECK(hover_resp.at("result").at("contents").at("kind") == "markdown");
    CHECK(value.find("The magic number.") != std::string::npos);

    lsp::write_message(proc.stdin_stream(),
                       {{"jsonrpc", "2.0"}, {"id", 3}, {"method", "shutdown"}});
    const auto shutdown_resp = UNWRAP(lsp::read_message(proc.stdout_stream(), std::cerr));
    CHECK(shutdown_resp.at("result").is_null());
    lsp::write_message(proc.stdin_stream(), {{"jsonrpc", "2.0"}, {"method", "exit"}});
    CHECK(UNWRAP(proc.close_stdin_and_wait()) == 0);
}

} // namespace ghoti::tests
