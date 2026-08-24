#include "driver/cmd/lsp/document_store.hh"

#include <filesystem>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <magic_enum/magic_enum.hpp>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/utility.hh>

#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"

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

    auto session{stdx::make_box<analysis_session>(loader_, error_stream_)};
    DISCARD(session->analyze(path));

    touched_modules results;
    for (const auto& [mod_path, mod] : session->get_manager()) {
        results.emplace_back(mod_path, to_lsp_diagnostics(*mod));
    }
    return results;
}

auto document_store::close(const std::filesystem::path& path) -> void { loader_.remove(path); }

} // namespace ghoti::lsp
