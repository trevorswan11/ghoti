#pragma once

#include <filesystem>
#include <ostream>

#include <gsl/pointers>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/sema/analyzer.hh"

namespace ghoti::lsp {

// One full parse+sema pass over an overlay-backed module graph rooted at an entry file.
//
// Must be heap allocated since `analyzer` holds a reference to `manager`
// that is only valid as long as this object's address never changes.
class analysis_session {
  public:
    analysis_session(mod::overlay_loader& loader, std::ostream& error_stream) noexcept;
    ~analysis_session() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(analysis_session)

    // Runs the pipeline for `entry_path` and returns its module regardless of errors
    [[nodiscard]] auto analyze(const std::filesystem::path& entry_path)
        -> stdx::result<gsl::not_null<mod::module*>, mod::diagnostic>;

    MAKE_DEDUCING_GETTER(manager);
    MAKE_DEDUCING_GETTER(analyzer);

  private:
    mod::module_manager manager_;
    sema::analyzer      analyzer_;
};

} // namespace ghoti::lsp
