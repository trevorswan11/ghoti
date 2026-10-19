#include "driver/cmd/lsp/document_store.hh"

#include <algorithm>
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

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "driver/cmd/lsp/snapshot.hh"

namespace ghoti::lsp {

auto document_store::register_known_root(const std::filesystem::path& path) -> bool {
    auto normalized{loader_.normalize(path)};
    if (!normalized) { return false; }
    if (std::ranges::find(known_roots_, *normalized) != known_roots_.end()) { return false; }
    known_roots_.emplace_back(std::move(*normalized));
    return true;
}

auto document_store::edit(const std::filesystem::path& path, std::string text) -> void {
    if (auto res{loader_.add(path, text)}; !res) {
        fmt::println(error_stream_,
                     "lsp: failed to overlay '{}': {}",
                     path.string(),
                     magic_enum::enum_name(res.error()));
        return;
    }
    register_known_root(path);
    ++revision_;
}

auto document_store::add_known_roots(std::vector<std::filesystem::path> paths) -> void {
    for (const auto& path : paths) { register_known_root(path); }
    ++revision_;
}

auto document_store::close(const std::filesystem::path& path) -> void {
    loader_.remove(path);
    ++revision_;
}

auto document_store::text_of(const std::filesystem::path& path) -> stdx::option<std::string> {
    auto text{loader_.load(path)};
    if (!text) { return stdx::none; }
    return std::move(*text);
}

auto document_store::inputs() const -> analysis_inputs {
    return {.revision = revision_, .files = loader_, .roots = known_roots_};
}

auto document_store::install(stdx::box<analysis_snapshot> snapshot) -> void {
    if (snapshot_ && (*snapshot_)->revision() >= snapshot->revision()) { return; }

    // Anything the previous analysis reached but this one doesn't has its diagnostics cleared
    pending_diagnostics_ = snapshot->diagnostics();
    if (snapshot_) {
        for (const auto& [mod_path, entries] : (*snapshot_)->workspace_index()) {
            if (!snapshot->workspace_index().contains(mod_path)) {
                pending_diagnostics_.emplace_back(mod_path, nlohmann::json::array());
            }
        }
    }
    snapshot_ = std::move(snapshot);
}

auto document_store::snapshot() noexcept -> stdx::option<analysis_snapshot&> {
    if (!snapshot_) { return stdx::none; }
    return **snapshot_;
}

auto document_store::is_current() const noexcept -> bool {
    return snapshot_ ? (*snapshot_)->revision() == revision_ : revision_ == 0;
}

auto document_store::take_pending_diagnostics() -> touched_modules {
    return std::exchange(pending_diagnostics_, {});
}

auto document_store::rebuild_if_dirty() -> void {
    if (is_current()) { return; }
    install(*analysis_snapshot::build(inputs(), parse_cache_, error_stream_));
}

auto document_store::current() -> analysis_snapshot& {
    rebuild_if_dirty();
    if (!snapshot_) { install(*analysis_snapshot::build(inputs(), parse_cache_, error_stream_)); }
    return **snapshot_;
}

auto document_store::update(const std::filesystem::path& path, std::string text)
    -> touched_modules {
    edit(path, std::move(text));
    rebuild_if_dirty();
    return take_pending_diagnostics();
}

auto document_store::seed_known_roots(std::vector<std::filesystem::path> paths) -> touched_modules {
    add_known_roots(std::move(paths));
    rebuild_if_dirty();
    return take_pending_diagnostics();
}

auto document_store::analyze(const std::filesystem::path& path)
    -> stdx::result<gsl::not_null<mod::module_t*>, mod::diagnostic> {
    if (auto module{current().module_of(path)}) { return module.get(); }
    return mod::make_mod_err(mod::error::PATH_DOES_NOT_EXIST);
}

auto document_store::manager() -> const mod::module_manager& { return current().manager(); }

auto document_store::workspace_symbols(std::string_view query) -> nlohmann::json {
    return current().workspace_symbols(query);
}

} // namespace ghoti::lsp
