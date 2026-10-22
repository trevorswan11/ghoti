#pragma once

#include <atomic>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/module/parse_cache.hh"
#include "driver/cmd/lsp/session.hh"
#include "driver/cmd/lsp/workspace_symbols.hh"

namespace ghoti::lsp {

using touched_modules = std::vector<std::pair<std::filesystem::path, nlohmann::json>>;
using workspace_map =
    ankerl::unordered_dense::map<std::filesystem::path, std::vector<workspace_symbol_entry>>;

// What one analysis reads, frozen at an editor revision so it can run on another thread
struct analysis_inputs {
    u64                                revision{0};
    mod::overlay_loader                files;
    std::vector<std::filesystem::path> roots;
};

// A finished analysis of one revision, which queries read while the next one is built.
//
// Shares nothing mutable with any other snapshot, so one thread can read it while another builds
// the next from the same parse cache. Pinned, since its session refers to its own loader
class analysis_snapshot {
  public:
    // Analyzes every root in `inputs`, borrowing unchanged files from `cache`.
    // None once `cancel` is set, since the analysis then stopped partway
    [[nodiscard]] static auto build(analysis_inputs                        inputs,
                                    mod::parse_cache&                      cache,
                                    std::ostream&                          error_stream,
                                    stdx::option<const std::atomic<bool>&> cancel = stdx::none)
        -> stdx::option<stdx::box<analysis_snapshot>>;

    analysis_snapshot(u64                 revision,
                      mod::overlay_loader files,
                      std::ostream&       error_stream,
                      mod::parse_cache&   cache) noexcept;
    ~analysis_snapshot() = default;
    MAKE_PINNED(analysis_snapshot);

    [[nodiscard]] auto revision() const noexcept -> u64 { return revision_; }

    // The analyzed module for `path`, if any root reached it
    [[nodiscard]] auto module_of(const std::filesystem::path& path) -> stdx::option<mod::module_t&>;

    [[nodiscard]] auto manager() -> const mod::module_manager& { return session_->get_manager(); }

    // The text `path` had in this revision, which every location in it is measured against
    [[nodiscard]] auto text_of(const std::filesystem::path& path) -> stdx::option<std::string>;

    // Top-level symbols of every reached module whose name contains `query`, case-insensitively
    [[nodiscard]] auto workspace_symbols(std::string_view query) -> nlohmann::json;

    [[nodiscard]] auto workspace_index() const noexcept -> const workspace_map& {
        return workspace_index_;
    }

    // Each reached module's diagnostics
    [[nodiscard]] auto diagnostics() const noexcept -> const touched_modules& {
        return diagnostics_;
    }

  private:
    u64                         revision_;
    mod::overlay_loader         files_;
    stdx::box<analysis_session> session_;
    workspace_map               workspace_index_;
    touched_modules             diagnostics_;
};

} // namespace ghoti::lsp
