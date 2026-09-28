#include "driver/cmd/build/run.hh"

#include <fmt/base.h>
#include <fmt/ostream.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto run_cmd::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    // Always build to a fresh temp path that is removed once the program has run.
    opts_.use_temp_executable_output("ghoti_run");

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze())};
    auto& analyzer{compilation.get_analyzer()};

    if (auto val_res{analyzer.validate_main_entry(*module)}; !val_res) {
        fmt::println(error_stream_, "{}", val_res.error());
        return stdx::err{clap::error::COMPILATION_FAILED};
    }

    auto emit_res{analyzer.emit_executable(gir_mod,
                                           opts_.target_opts,
                                           opts_.opt_opts,
                                           opts_.output_path,
                                           compilation.linker_options())};
    if (!emit_res) { return build::report_codegen_error(error_stream_, emit_res.error()); }
    return build::run_built_executable(opts_, error_stream_, true);
}

} // namespace ghoti::cmd
