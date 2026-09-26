#include "driver/cmd/build/library.hh"

#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto build_lib::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze())};
    auto& analyzer{compilation.get_analyzer()};

    TRY(opts_.emit_debug_artifacts(analyzer, gir_mod, error_stream_));

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

    if (!emit_res) { return build::report_codegen_error(error_stream_, emit_res.error()); }
    return {};
}

} // namespace ghoti::cmd
