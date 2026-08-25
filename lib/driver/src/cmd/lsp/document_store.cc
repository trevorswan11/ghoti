#include "driver/cmd/lsp/document_store.hh"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/utility.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "driver/cmd/lsp/workspace_symbols.hh"
#include "support/path_utils.hh"
#include "support/string_utils.hh"

namespace ghoti::lsp {

auto document_store::register_known_root(const std::filesystem::path& path) -> bool {
    auto normalized{loader_.normalize(path)};
    if (!normalized) { return false; }
    if (std::ranges::find(known_roots_, *normalized) != known_roots_.end()) { return false; }
    known_roots_.emplace_back(std::move(*normalized));
    return true;
}

auto document_store::rebuild() -> void {
    session_ = stdx::make_box<analysis_session>(loader_, error_stream_);
    for (const auto& root : known_roots_) {
        if (auto res{session_->analyze(root)}; !res) {
            fmt::println(error_stream_,
                         "lsp: failed to reanalyze known root '{}': {}",
                         root.string(),
                         res.error());
        }
    }

    pending_diagnostics_.clear();
    for (const auto& [mod_path, mod] : session_->get_manager()) {
        workspace_index_[mod_path] = module_workspace_symbols(*mod);
        pending_diagnostics_.emplace_back(mod_path, to_lsp_diagnostics(*mod));
    }

    last_rebuild_ = std::chrono::steady_clock::now();
    dirty_        = false;
}

auto document_store::rebuild_if_dirty() -> void {
    if (dirty_) { rebuild(); }
}

auto document_store::take_pending_diagnostics() -> touched_modules {
    return std::exchange(pending_diagnostics_, {});
}

auto document_store::update(const std::filesystem::path& path, std::string text)
    -> touched_modules {
    if (auto res{loader_.add(path, text)}; !res) {
        fmt::println(error_stream_,
                     "lsp: failed to overlay '{}': {}",
                     path.string(),
                     magic_enum::enum_name(res.error()));
        return {};
    }
    register_known_root(path);

    // Rebuild from scratch and reanalyze every known root, not just `path`
    rebuild();
    return take_pending_diagnostics();
}

auto document_store::update_throttled(const std::filesystem::path& path, std::string text)
    -> touched_modules {
    if (auto res{loader_.add(path, text)}; !res) {
        fmt::println(error_stream_,
                     "lsp: failed to overlay '{}': {}",
                     path.string(),
                     magic_enum::enum_name(res.error()));
        return {};
    }
    register_known_root(path);
    dirty_ = true;

    if (std::chrono::steady_clock::now() - last_rebuild_ < throttle_interval_) { return {}; }

    rebuild();
    return take_pending_diagnostics();
}

auto document_store::seed_known_roots(std::vector<std::filesystem::path> paths) -> touched_modules {
    for (const auto& path : paths) { register_known_root(path); }
    rebuild();
    return take_pending_diagnostics();
}

auto document_store::close(const std::filesystem::path& path) -> void { loader_.remove(path); }

auto document_store::text_of(const std::filesystem::path& path) -> stdx::option<std::string> {
    auto text{loader_.load(path)};
    if (!text) { return stdx::none; }
    return std::move(*text);
}

auto document_store::analyze(const std::filesystem::path& path)
    -> stdx::result<gsl::not_null<mod::module*>, mod::diagnostic> {
    rebuild_if_dirty();
    return session_->analyze(path);
}

auto document_store::manager() -> const mod::module_manager& {
    rebuild_if_dirty();
    return session_->get_manager();
}

auto document_store::workspace_symbols(std::string_view query) -> nlohmann::json {
    rebuild_if_dirty();

    auto out = nlohmann::json::array();

    for (const auto& [path, entries] : workspace_index_) {
        const auto uri{path_utils::path_to_uri(path)};
        for (const auto& entry : entries) {
            if (!query.empty() && !string_utils::contains_ci(entry.name, query)) { continue; }
            out.push_back({
                {"name", entry.name},
                {"kind", std::to_underlying(entry.kind)},
                {
                    "location",
                    {
                        {"uri", uri},
                        {"range", range_of(entry.range)},
                    },
                },
            });
        }
    }

    return out;
}

} // namespace ghoti::lsp
