#include "driver/cmd/build_lib.hh"

#include <filesystem>
#include <system_error>

#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "ghoti/config.h"

namespace ghoti::cmd {

auto build_lib::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    std::error_code ec;
    if (!std::filesystem::exists(opts_.input_path, ec)) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("file '{}' not found", opts_.input_path.string()),
                                 clap::error::FILE_NOT_FOUND);
    }

    opts_.make_path_relative();
    mod::file_loader    loader;
    mod::module_manager manager{loader};
    TRY(opts_.setup_module_manager(manager, error_stream_));

    sema::analyzer analyzer{manager, error_stream_, true, opts_.target_opts};
    auto           module{TRY(opts_.analyze(analyzer, manager, error_stream_))};

    auto gir_mod{analyzer.emit_gir(*module)};
    if (module->is_poisoned()) { return stdx::err{clap::error::COMPILATION_FAILED}; }

    stdx::result<void, codegen::diagnostic> emit_res;
    if (opts_.dynamic) {
        emit_res = analyzer.emit_dynamic_library(gir_mod,
                                                 opts_.target_opts,
                                                 opts_.opt_opts,
                                                 opts_.output_path,
                                                 {
                                                     .objects       = opts_.extra_objects,
                                                     .library_paths = opts_.library_paths,
                                                     .libraries     = opts_.libraries,
                                                 });
    } else {
        emit_res = analyzer.emit_static_library(gir_mod,
                                                opts_.target_opts,
                                                opts_.opt_opts,
                                                opts_.output_path,
                                                {.objects = opts_.extra_objects});
    }

    if (!emit_res) {
        return clap::fatal_error(error_stream_,
                                 emit_res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR),
                                 clap::error::COMPILATION_FAILED);
    }
    return {};
}

} // namespace ghoti::cmd
