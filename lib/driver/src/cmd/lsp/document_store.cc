#include "driver/cmd/lsp/document_store.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/utility.hh>

#include "compiler/module/error.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "driver/cmd/lsp/workspace_symbols.hh"
#include "support/path_utils.hh"
#include "support/string_utils.hh"

namespace ghoti::lsp {

auto document_store::update(const std::filesystem::path& path, std::string text)
    -> touched_modules {
    if (auto res{loader_.add(path, text)}; !res) {
        fmt::println(error_stream_,
                     "lsp: failed to overlay '{}': {}",
                     path.string(),
                     magic_enum::enum_name(res.error()));
        return {};
    }

    auto result{analyze(path)};
    if (!result) { return {}; }

    touched_modules results;
    for (const auto& [mod_path, mod] : result->first->get_manager()) {
        results.emplace_back(mod_path, to_lsp_diagnostics(*mod));
        workspace_index_[mod_path] = module_workspace_symbols(*mod);
    }
    return results;
}

auto document_store::close(const std::filesystem::path& path) -> void { loader_.remove(path); }

auto document_store::text_of(const std::filesystem::path& path) -> stdx::option<std::string> {
    auto text{loader_.load(path)};
    if (!text) { return stdx::none; }
    return std::move(*text);
}

auto document_store::analyze(const std::filesystem::path& path)
    -> stdx::result<query_result, mod::diagnostic> {
    auto session{stdx::make_box<analysis_session>(loader_, error_stream_)};
    auto module_result{session->analyze(path)};
    if (!module_result) { return stdx::err{module_result.error()}; }

    const auto entry_module{*module_result};
    return query_result{std::move(session), entry_module};
}

auto document_store::workspace_symbols(std::string_view query) const -> nlohmann::json {
    auto out = nlohmann::json::array();

    for (const auto& [path, entries] : workspace_index_) {
        const auto uri{path_utils::path_to_uri(path)};
        for (const auto& entry : entries) {
            if (!query.empty() && !string_utils::contains_ci(entry.name, query)) { continue; }
            out.push_back({
                {"name", entry.name},
                {"kind", std::to_underlying(entry.kind)},
                {"location", {{"uri", uri}, {"range", range_of(entry.range)}}},
            });
        }
    }

    return out;
}

} // namespace ghoti::lsp
