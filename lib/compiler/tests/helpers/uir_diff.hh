#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/source_loader.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/context.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"

namespace ghoti::tests::helpers {

// The build both pipelines analyze a program for
struct pipeline_settings {
    codegen::target_options target_opts{};
    bool                    runtime_safety{true};
    bool                    testing{false};
    sema::optimize_mode     build_mode{sema::optimize_mode::DEBUG};
};

enum class uir_diff_status : u8 {
    MATCH,
    // UIR Sema stopped at a construct it doesn't analyze yet
    UNSUPPORTED,
    // The old pipeline emitted the program but UIR Sema reported errors
    REJECTED,
    // An immutable `let` initialized by a call, which the old emitter folds and UIR doesn't
    FOLD_PLACEMENT,
    SEMANTICS_CHANGE,
    // Only parameterized impl methods nothing uses differ, which the old pipeline expands
    ON_USE_IMPL,
    // The old pipeline's GIR shows a bug `docs/uir/known_legacy_bugs.md` records
    LEGACY_BUG,
    GIR_DIFF,
    ASTGEN_FAILED,
    // The old pipeline rejected the program, so there is nothing to compare against
    LEGACY_REJECTED,
};

[[nodiscard]] auto uir_diff_status_name(uir_diff_status status) noexcept -> std::string_view;

struct uir_diff {
    uir_diff_status status;
    std::string     detail{};
};

using library_list = gsl::span<const std::pair<std::string, std::filesystem::path>>;

// Analyzes `root` with both pipelines from fresh module managers and compares their normalized GIR
[[nodiscard]] auto diff_pipelines(mod::source_loader&          loader,
                                  const std::filesystem::path& root,
                                  library_list                 libraries,
                                  const pipeline_settings&     settings) -> uir_diff;

// Appends the outcome to `GHOTI_TEST_UIR_DIFF_REPORT` when that names a file
auto report_uir_diff(const uir_diff& diff) -> void;

// UIR Sema's outcome for one program, with everything it points into kept alive
struct uir_sema_run {
    stdx::box<mod::memory_loader>    loader;
    stdx::box<mod::module_manager>   modules;
    stdx::box<sema::analyzer>        analyzer;
    std::vector<uir::lowered_module> lowered;
    stdx::option<uir::sema::outcome> out;
};

// Lowers and analyzes `source` through UIR Sema alone
[[nodiscard]] auto run_uir_sema(std::string_view source, const pipeline_settings& settings = {})
    -> stdx::box<uir_sema_run>;
// Lowers and analyzes the program rooted at `root` through UIR Sema alone. `loader` must outlive
// the run
[[nodiscard]] auto run_uir_sema_files(mod::source_loader&          loader,
                                      const std::filesystem::path& root,
                                      const pipeline_settings&     settings = {})
    -> stdx::box<uir_sema_run>;
// The normalized GIR of a program UIR Sema must accept
[[nodiscard]] auto uir_gir(std::string_view source) -> std::string;

} // namespace ghoti::tests::helpers
