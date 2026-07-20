#include "driver/cmd/build_obj.hh"

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

namespace ghoti::cmd {

auto build_obj::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();

    if (!std::filesystem::exists(input_path_)) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("file '{}' not found", input_path_.string()),
                                 clap::error::FILE_NOT_FOUND);
    }

    std::filesystem::path entry_path{input_path_};
    if (entry_path.is_absolute()) {
        std::error_code ec;
        auto            rel{std::filesystem::relative(entry_path, ec)};
        if (!ec && !rel.empty()) { entry_path = rel; }
    }

    mod::file_loader    loader;
    mod::module_manager manager{loader};

    sema::analyzer analyzer{manager, error_stream_, true};
    if (!analyzer.analyze(entry_path)) { return stdx::err{clap::error::COMPILATION_FAILED}; }

    auto module_result{manager.try_get_file_module(entry_path)};
    if (!module_result) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("failed to retrieve module '{}'", entry_path.string()),
                                 clap::error::COMPILATION_FAILED);
    }

    auto module{*module_result};
    if (module->is_poisoned() || module->is_errored()) {
        return stdx::err{clap::error::COMPILATION_FAILED};
    }

    auto gir_mod{analyzer.emit_gir(*module)};
    if (module->is_poisoned()) { return stdx::err{clap::error::COMPILATION_FAILED}; }

    auto emit_res{analyzer.emit_object(gir_mod, target_opts_, opt_opts_, output_path_)};
    if (!emit_res) {
        return clap::fatal_error(
            error_stream_,
            emit_res.error().get_message().value_or("i don't know what went wrong"),
            clap::error::COMPILATION_FAILED);
    }
    return {};
}

} // namespace ghoti::cmd
