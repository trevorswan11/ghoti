#include "driver/cmd/lsp/session.hh"

#include <filesystem>
#include <ostream>

#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/module/error.hh"
#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/module/stdlib.hh"

namespace ghoti::lsp {

analysis_session::analysis_session(mod::overlay_loader& loader, std::ostream& error_stream) noexcept
    : manager_{loader}, analyzer_{manager_, error_stream, false} {
    PROFILE_FUNCTION();
    const auto stdlib_path{mod::find_stdlib()};
    if (!stdlib_path) {
        fmt::println(error_stream, "could not locate the ghoti standard library");
        return;
    }

    if (auto res{manager_.add_library_module("std", *stdlib_path)}; !res) {
        fmt::println(error_stream, "failed to register stdlib: {}", res.error());
    }
}

auto analysis_session::analyze(const std::filesystem::path& entry_path)
    -> stdx::result<gsl::not_null<mod::module*>, mod::diagnostic> {
    PROFILE_FUNCTION();
    // A poisoned/errored module is still returned so its diagnostics can be reported
    DISCARD(analyzer_.analyze(entry_path));
    return manager_.try_get_file_module(entry_path);
}

} // namespace ghoti::lsp
