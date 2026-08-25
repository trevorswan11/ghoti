#include "driver/cmd/lsp/workspace_scan.hh"

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <stdx/string.hh>
#include <stdx/types.hh>

namespace ghoti::lsp {

namespace {

auto scan_root(const std::filesystem::path&        root,
               const std::vector<std::string>&     excludes,
               usize                               max_files,
               std::vector<std::filesystem::path>& out) -> void {
    std::error_code ec;
    // Resolve symlinks in the root once so every yielded entry is consistently canonical
    const auto  canonical_root{std::filesystem::weakly_canonical(root, ec)};
    const auto& effective_root{ec ? root : canonical_root};
    ec.clear();

    auto it{std::filesystem::recursive_directory_iterator{
        effective_root, std::filesystem::directory_options::skip_permission_denied, ec}};
    const std::filesystem::recursive_directory_iterator end{};
    if (ec) { return; }

    while (it != end) {
        if (out.size() >= max_files) { return; }

        const auto& entry_path{it->path()};
        if (it->is_directory(ec)) {
            if (!ec && std::ranges::contains(excludes, entry_path.filename().string())) {
                it.disable_recursion_pending();
            }
        } else if (entry_path.has_extension()) {
            auto ext{entry_path.extension().string()};
            stdx::string::inplace_lower(ext);
            if (ext == ".gh") { out.emplace_back(entry_path); }
        }

        it.increment(ec);
        if (ec) { return; }
    }
}

} // namespace

auto discover_workspace_files(const std::vector<std::filesystem::path>& roots,
                              const std::vector<std::string>&           excludes,
                              usize max_files) -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> out;
    for (const auto& root : roots) {
        if (out.size() >= max_files) { break; }
        scan_root(root, excludes, max_files, out);
    }
    return out;
}

} // namespace ghoti::lsp
