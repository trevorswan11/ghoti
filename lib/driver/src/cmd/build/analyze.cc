#include "driver/cmd/build/analyze.hh"

#include <fmt/base.h>
#include <fmt/ostream.h>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/options.hh"

namespace ghoti::cmd {

auto analyze_cmd::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    build::compilation compilation{opts_, error_stream_};
    auto [module, gir_mod]{TRY(compilation.analyze(as_test_))};
    auto& analyzer{compilation.get_analyzer()};

    // A hand-written `test_runner` must match the entry `ghoti test` would force
    if (as_test_) {
        if (auto val_res{analyzer.validate_test_entry(*module)}; !val_res) {
            fmt::println(error_stream_, "{}", val_res.error());
            return stdx::err{clap::error::COMPILATION_FAILED};
        }
    }

    const auto artifact{as_test_ ? sema::build_artifact::TEST_EXECUTABLE
                                 : sema::build_artifact::OBJECT};
    return opts_.emit_debug_artifacts(analyzer, gir_mod, error_stream_, artifact);
}

} // namespace ghoti::cmd
