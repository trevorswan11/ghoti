#include "driver/cmd/build/test.hh"

#include <fmt/base.h>
#include <fmt/ostream.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto test_cmd::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    // Without an explicit `-o`, always build to a fresh temp path that is cleaned post-run
    if (!opts_.output_explicit) { opts_.use_temp_executable_output("ghoti_test"); }

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze(true))};
    auto& analyzer{compilation.get_analyzer()};

    // A hand-written `test_runner` override must match the forced entry signature.
    if (auto val_res{analyzer.validate_test_entry(*module)}; !val_res) {
        fmt::println(error_stream_, "{}", val_res.error());
        return stdx::err{clap::error::COMPILATION_FAILED};
    }

    TRY(opts_.emit_debug_artifacts(
        analyzer, gir_mod, error_stream_, sema::build_artifact::TEST_EXECUTABLE));

    auto emit_res{analyzer.emit_test_executable(gir_mod,
                                                opts_.target_opts,
                                                opts_.opt_opts,
                                                opts_.output_path,
                                                {
                                                    .objects       = opts_.extra_objects,
                                                    .library_paths = opts_.library_paths,
                                                    .libraries     = opts_.libraries,
                                                })};
    if (!emit_res) { return build::report_codegen_error(error_stream_, emit_res.error()); }
    return build::run_built_executable(opts_, error_stream_, !opts_.output_explicit);
}

} // namespace ghoti::cmd
