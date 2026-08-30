#include "driver/cmd/build/test.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "ghoti/config.h"
#include "support/subprocess.hh"
#include "support/tempfile.hh"

namespace ghoti::cmd {

auto test_cmd::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    std::error_code ec;
    if (!std::filesystem::exists(opts_.input_path, ec)) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("file '{}' not found", opts_.input_path.string()),
                                 clap::error::FILE_NOT_FOUND);
    }

    opts_.make_path_relative();
    if (opts_.output_path.empty()) {
        const auto default_ext{codegen::get_default_output_extension(
            codegen::output_type::EXECUTABLE, opts_.target_opts.triple_str)};
        const auto temp_stem{tempfile::make_temp_path("ghoti_test")};
        opts_.output_path = temp_stem;
        if (!default_ext.empty()) { opts_.output_path.replace_extension(default_ext); }
    }

    if (opts_.output_path.is_relative()) {
        std::error_code abs_ec;
        if (auto abs{std::filesystem::absolute(opts_.output_path, abs_ec)}; !abs_ec) {
            opts_.output_path = std::move(abs);
        }
    }
    mod::file_loader    loader;
    mod::module_manager manager{loader};
    TRY(opts_.setup_module_manager(manager, error_stream_));

    sema::analyzer analyzer{
        manager,
        error_stream_,
        true,
        opts_.target_opts,
        false,
        opts_.runtime_safety,
    };

    auto module{TRY(opts_.analyze(analyzer, manager, error_stream_))};
    auto gir_mod{analyzer.emit_gir(*module, /*for_test_executable=*/true)};
    if (module->is_poisoned()) { return stdx::err{clap::error::COMPILATION_FAILED}; }

    auto emit_res{analyzer.emit_test_executable(gir_mod,
                                                opts_.target_opts,
                                                opts_.opt_opts,
                                                opts_.output_path,
                                                {
                                                    .objects       = opts_.extra_objects,
                                                    .library_paths = opts_.library_paths,
                                                    .libraries     = opts_.libraries,
                                                })};
    if (!emit_res) {
        return clap::fatal_error(error_stream_,
                                 emit_res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR),
                                 clap::error::COMPILATION_FAILED);
    }

    // Execute the test executable
    stdx::option<u32> exit_code_opt;
    {
        tempfile        test_exe{std::in_place, opts_.output_path};
        const mock_argv args{opts_.output_path.string()};
        exit_code_opt = spawn_child(args);
    }

    if (!exit_code_opt) {
        return clap::fatal_error(
            error_stream_, "failed to execute test runner binary", clap::error::UNEXPECTED_ERROR);
    }

    const auto exit_code{*exit_code_opt};
    if (exit_code != 0) { return stdx::err{static_cast<clap::error>(exit_code)}; }
    return {};
}

} // namespace ghoti::cmd
