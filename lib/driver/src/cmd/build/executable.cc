#include "driver/cmd/build/executable.hh"

#include <fmt/base.h>
#include <fmt/ostream.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto build_exe::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze())};
    auto& analyzer{compilation.get_analyzer()};

    // Validate that root module contains valid 'pub const main := fn(args: [][:0]u8): void'
    if (auto val_res{analyzer.validate_main_entry(*module)}; !val_res) {
        fmt::println(error_stream_, "{}", val_res.error());
        return stdx::err{clap::error::COMPILATION_FAILED};
    }

    TRY(opts_.emit_debug_artifacts(
        analyzer, gir_mod, error_stream_, sema::build_artifact::EXECUTABLE));

    auto emit_res{analyzer.emit_executable(gir_mod,
                                           opts_.target_opts,
                                           opts_.opt_opts,
                                           opts_.output_path,
                                           compilation.linker_options())};
    if (!emit_res) { return build::report_codegen_error(error_stream_, emit_res.error()); }
    return {};
}

} // namespace ghoti::cmd
