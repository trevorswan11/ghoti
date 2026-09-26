#include "driver/cmd/build/object.hh"

#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto build_obj::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze())};
    auto& analyzer{compilation.get_analyzer()};
    TRY(opts_.emit_debug_artifacts(
        analyzer, gir_mod, error_stream_, sema::build_artifact::OBJECT));

    auto emit_res{
        analyzer.emit_object(gir_mod, opts_.target_opts, opts_.opt_opts, opts_.output_path)};
    if (!emit_res) { return build::report_codegen_error(error_stream_, emit_res.error()); }
    return {};
}

} // namespace ghoti::cmd
