#include "driver/cmd/build/options.hh"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/build/compiler_rt.hh"
#include "ghoti/config.h"
#include "support/path_utils.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"

namespace ghoti::cmd::build {

namespace {

[[nodiscard]] auto parse_deprecation_policy(std::string_view name) -> sema::deprecation_policy {
    if (name == "error") { return sema::deprecation_policy::DENY; }
    if (name == "ignore") { return sema::deprecation_policy::ALLOW; }
    return sema::deprecation_policy::WARN;
}

} // namespace

auto add_deprecated_option(CLI::App* subcmd, raw_options& opts) -> void {
    subcmd
        ->add_option("--deprecated",
                     opts.deprecated,
                     "How a use of a @[deprecated] declaration is reported (warn, error, ignore)")
        ->check(CLI::IsMember({"warn", "error", "ignore"}))
        ->default_val(opts.deprecated);
}

auto options::process_raw(const raw_options&   raw,
                          codegen::output_type type,
                          std::ostream&        error_stream) -> stdx::result<options, clap::error> {
    codegen::optimizer_options opt_opts{
        .debug_logging = raw.debug_passes,
        .time_passes   = raw.time_passes,
    };

    if (raw.opt_level_str && raw.release) {
        clap::warn_error(
            error_stream,
            fmt::format("--release is overridden by the explicit -O {}", *raw.opt_level_str));
    }
    if (raw.opt_level_str) {
        if (auto level{codegen::parse_opt_level(*raw.opt_level_str)}) {
            opt_opts.level = *level;
        } else {
            return clap::fatal_error(
                error_stream,
                fmt::format("invalid optimization level '{}'", *raw.opt_level_str),
                clap::error::INVALID_OPTIMIZATION);
        }
    } else if (raw.release) {
        opt_opts.level = codegen::opt_level::O2;
    } else {
        opt_opts.level = codegen::opt_level::O0;
    }

    std::filesystem::path input_path{raw.input};
    std::filesystem::path output_path;
    if (raw.output.empty()) {
        output_path = input_path;
        const auto default_ext{codegen::get_default_output_extension(
            type, raw.target.empty() ? stdx::none : stdx::option<std::string_view>{raw.target})};
        if (default_ext.empty()) {
            output_path.replace_extension("");
        } else {
            output_path.replace_extension(default_ext);
        }
    } else {
        output_path = raw.output;
    }

    codegen::target_options target_opts{
        .triple_str = raw.target.empty() ? stdx::none : stdx::option<std::string>{raw.target},
        .cpu        = raw.cpu.empty() ? "generic" : raw.cpu,
        .features   = raw.features,
        .level      = opt_opts.level,
    };

    std::vector<module_binding> modules;
    for (const auto& raw_mod : raw.module_raw_args) {
        const auto comma_pos{raw_mod.find(',')};
        if (comma_pos == std::string::npos || comma_pos == 0 || comma_pos + 1 >= raw_mod.size()) {
            return clap::fatal_error(
                error_stream,
                fmt::format("invalid module format '{}', expected '<name>,<path>'", raw_mod),
                clap::error::INVALID_MODULE_SPEC);
        }
        std::string name{stdx::string::substr(raw_mod, 0, comma_pos)};
        std::string mod_path{stdx::string::substr(raw_mod, comma_pos + 1)};
        modules.emplace_back(std::move(name), std::filesystem::path{std::move(mod_path)});
    }

    std::vector<std::filesystem::path> extra_objects;
    extra_objects.reserve(raw.extra_objects.size());
    for (const auto& obj : raw.extra_objects) { extra_objects.emplace_back(obj); }

    std::vector<std::filesystem::path> library_paths;
    library_paths.reserve(raw.library_paths.size());
    for (const auto& dir : raw.library_paths) { library_paths.emplace_back(dir); }

    stdx::option<std::filesystem::path> emit_gir_path;
    if (!raw.emit_gir_path.empty()) { emit_gir_path.emplace(raw.emit_gir_path); }
    stdx::option<std::filesystem::path> emit_llvm_ir_path;
    if (!raw.emit_llvm_ir_path.empty()) { emit_llvm_ir_path.emplace(raw.emit_llvm_ir_path); }
    stdx::option<std::filesystem::path> emit_asm_path;
    if (!raw.emit_asm_path.empty()) { emit_asm_path.emplace(raw.emit_asm_path); }

    return options{
        .input_path        = std::move(input_path),
        .output_path       = std::move(output_path),
        .target_opts       = std::move(target_opts),
        .opt_opts          = std::move(opt_opts),
        .modules           = std::move(modules),
        .extra_objects     = std::move(extra_objects),
        .library_paths     = std::move(library_paths),
        .libraries         = raw.libraries,
        .forwarded_args    = raw.forwarded_args,
        .dynamic           = raw.dynamic,
        .runtime_safety    = !raw.unsafe,
        .deprecated_policy = parse_deprecation_policy(raw.deprecated),
        .output_explicit   = !raw.output.empty(),
        .emit_gir_path     = std::move(emit_gir_path),
        .emit_llvm_ir_path = std::move(emit_llvm_ir_path),
        .emit_asm_path     = std::move(emit_asm_path),
    };
}

auto options::use_temp_executable_output(std::string_view tag) -> void {
    const auto default_ext{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE,
                                                                 target_opts.triple_str)};
    output_path = tempfile::make_temp_path(tag);
    if (!default_ext.empty()) { output_path.replace_extension(default_ext); }
}

auto options::make_output_path_absolute() -> void {
    if (!output_path.is_relative()) { return; }
    std::error_code ec;
    if (auto abs{std::filesystem::absolute(output_path, ec)}; !ec) { output_path = std::move(abs); }
}

compilation::compilation(options& opts, std::ostream& error_stream)
    : opts_{opts}, error_stream_{error_stream}, manager_{loader_},
      analyzer_{manager_, error_stream_, true, opts_.target_opts, false, opts_.runtime_safety} {
    analyzer_.set_deprecation_policy(opts_.deprecated_policy);
    // Spawned children resolve a bare relative name via PATH, so pin the output path first
    opts_.make_output_path_absolute();
}

auto compilation::analyze(bool for_test_executable) -> stdx::result<analyzed_module, clap::error> {
    TRY(validate_input_path());
    TRY(ensure_output_directory());
    TRY(setup_module_manager());

    auto gir_mod_res{analyzer_.analyze(opts_.input_path, for_test_executable)};
    if (!gir_mod_res) {
        return clap::fatal_error(error_stream_,
                                 gir_mod_res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR),
                                 clap::error::COMPILATION_FAILED);
    }

    auto module_result{manager_.try_get_file_module(opts_.input_path)};
    if (!module_result) {
        return clap::fatal_error(
            error_stream_,
            fmt::format("failed to retrieve module '{}'", opts_.input_path.string()),
            clap::error::COMPILATION_FAILED);
    }

    auto module{*module_result};
    if (module->is_poisoned() || module->is_errored()) {
        return stdx::err{clap::error::COMPILATION_FAILED};
    }
    // A `-m`-registered library module (or a transitively-imported one) is parsed lazily and
    // may fail independently of the entry module's own state
    if (manager_.any_errored()) {
        manager_.print_all_diagnostics(error_stream_);
        return stdx::err{clap::error::COMPILATION_FAILED};
    }
    return std::make_pair(module, std::move(*gir_mod_res));
}

auto compilation::linker_options() const -> codegen::extra_linker_options {
    return {
        .objects       = opts_.extra_objects,
        .library_paths = opts_.library_paths,
        .libraries     = opts_.libraries,
        .builtins      = resolve_compiler_rt(opts_.target_opts),
    };
}

auto compilation::validate_input_path() -> stdx::result<void, clap::error> {
    if (!path_utils::exists(opts_.input_path)) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("file '{}' not found", opts_.input_path.string()),
                                 clap::error::FILE_NOT_FOUND);
    }
    if (!std::filesystem::is_regular_file(opts_.input_path)) {
        return clap::fatal_error(
            error_stream_,
            fmt::format("input path '{}' is not a regular file", opts_.input_path.string()),
            clap::error::FILE_NOT_FOUND);
    }

    if (auto rel{path_utils::make_relative(opts_.input_path)}) {
        opts_.input_path = std::move(*rel);
    }
    return {};
}

auto compilation::ensure_output_directory() -> stdx::result<void, clap::error> {
    const auto parent{opts_.output_path.parent_path()};
    if (parent.empty()) { return {}; }
    std::error_code ec;
    if (std::filesystem::is_directory(parent, ec)) { return {}; }
    if (path_utils::exists(parent)) {
        return clap::fatal_error(
            error_stream_,
            fmt::format("output location '{}' is not a directory", parent.string()),
            clap::error::IO_ERROR);
    }
    if (!std::filesystem::create_directories(parent, ec) || ec) {
        return clap::fatal_error(error_stream_,
                                 fmt::format("could not create output directory '{}': {}",
                                             parent.string(),
                                             ec.message()),
                                 clap::error::IO_ERROR);
    }
    return {};
}

auto compilation::setup_module_manager() -> stdx::result<void, clap::error> {
    if (const auto stdlib_path{mod::find_stdlib()}) {
        if (auto res{manager_.add_library_module("std", *stdlib_path)}; !res) {
            return clap::fatal_error(
                error_stream_,
                fmt::format("failed to register stdlib: {}",
                            res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR)),
                clap::error::COMPILATION_FAILED);
        }
    } else {
        return clap::fatal_error(
            error_stream_,
            fmt::format("could not locate the ghoti standard library; set {} to its std.gh "
                        "path, or run from within a source checkout",
                        GHOTI_STDLIB_ENV),
            clap::error::COMPILATION_FAILED);
    }

    for (const auto& mod : opts_.modules) {
        if (!path_utils::exists(mod.path)) {
            return clap::fatal_error(
                error_stream_,
                fmt::format("module '{}' root path '{}' not found", mod.name, mod.path.string()),
                clap::error::FILE_NOT_FOUND);
        }

        if (auto res{manager_.add_library_module(mod.name, mod.path)}; !res) {
            return clap::fatal_error(
                error_stream_,
                fmt::format("failed to register module '{}': {}",
                            mod.name,
                            res.error().get_message().value_or(GHOTI_UNKNOWN_ERROR)),
                clap::error::COMPILATION_FAILED);
        }
    }
    return {};
}

auto report_codegen_error(std::ostream& error_stream, const codegen::diagnostic& diag)
    -> stdx::err<clap::error> {
    return clap::fatal_error(error_stream,
                             diag.get_message().value_or(GHOTI_UNKNOWN_ERROR),
                             clap::error::COMPILATION_FAILED);
}

auto run_built_executable(const options& opts, std::ostream& error_stream, bool cleanup_output)
    -> stdx::result<void, clap::error> {
    std::vector<std::string> child_argv{opts.output_path.string()};
    child_argv.insert(child_argv.end(), opts.forwarded_args.begin(), opts.forwarded_args.end());

    stdx::option<tempfile> built_exe;
    if (cleanup_output) { built_exe.emplace(std::in_place, opts.output_path); }

    // The program (or test suite) owns its runtime; never impose the default spawn timeout on it.
    const auto exit_code_opt{
        spawn_child(mock_argv{std::move(child_argv)}, std::chrono::milliseconds::max())};
    if (!exit_code_opt) {
        return clap::fatal_error(error_stream,
                                 fmt::format("failed to execute '{}'", opts.output_path.string()),
                                 clap::error::UNEXPECTED_ERROR);
    }

    if (const auto exit_code{*exit_code_opt}; exit_code != 0) {
        return stdx::err{static_cast<clap::error>(exit_code)};
    }
    return {};
}

auto setup_flags(CLI::App* subcmd, raw_options& opts, stdx::option<std::string_view> output_desc)
    -> void {
    if (output_desc) { subcmd->add_option("-o,--output", opts.output, std::string{*output_desc}); }
    subcmd->add_option(
        "-m,--module", opts.module_raw_args, "Register a library module (format: <name>,<path>)");
    subcmd->add_option("--object", opts.extra_objects, "Link precompiled object file or library");
    subcmd->add_option(
        "-L,--library-path", opts.library_paths, "Add directory to library search paths");
    subcmd->add_option("-l,--library", opts.libraries, "Link against library name");
    subcmd->add_option("--target", opts.target, "Target triple");
    subcmd->add_option("--cpu", opts.cpu, "Target CPU architecture")->default_val(opts.cpu);
    subcmd->add_option("--features", opts.features, "Target CPU features");
    subcmd->add_option(
        "-O,--opt-level", opts.opt_level_str, "Optimization level (0, 1, 2, 3, s, z)");
    subcmd->add_flag("--release", opts.release, "Build in release mode (defaults to -O2)")
        ->default_val(opts.release);
    subcmd
        ->add_flag("--debug-passes",
                   opts.debug_passes,
                   "Enable debug logging and IR printing after passes")
        ->default_val(opts.debug_passes);
    subcmd->add_flag("--time-passes", opts.time_passes, "Enable pass execution timing report")
        ->default_val(opts.time_passes);
    subcmd->add_flag("--unsafe", opts.unsafe, "Disable all runtime safety checks")
        ->default_val(opts.unsafe);
    add_deprecated_option(subcmd, opts);
    subcmd
        ->add_option("--emit-gir", opts.emit_gir_path, "Write the GIR dump to the given file path")
        ->type_name("FILE");
    subcmd
        ->add_option(
            "--emit-llvm-ir", opts.emit_llvm_ir_path, "Write the LLVM IR to the given file path")
        ->type_name("FILE");
    subcmd
        ->add_option(
            "--emit-asm", opts.emit_asm_path, "Write the native assembly to the given file path")
        ->type_name("FILE");
}

auto options::emit_debug_artifacts(sema::analyzer&      analyzer,
                                   gir::module&         gir_mod,
                                   std::ostream&        error_stream,
                                   sema::build_artifact artifact) const
    -> stdx::result<void, clap::error> {
    const auto write_file{[&](const std::filesystem::path& path,
                              std::string_view             contents,
                              std::string_view label) -> stdx::result<void, clap::error> {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return clap::fatal_error(
                error_stream,
                fmt::format("could not open '{}' to write {}", path.string(), label),
                clap::error::IO_ERROR);
        }
        fmt::print(out, "{}", contents);
        if (!out) {
            return clap::fatal_error(
                error_stream,
                fmt::format("failed to write {} to '{}'", label, path.string()),
                clap::error::IO_ERROR);
        }
        return {};
    }};

    if (emit_gir_path) { TRY(write_file(*emit_gir_path, gir_mod.to_string(), "GIR")); }

    if (emit_llvm_ir_path) {
        auto ir{analyzer.emit_llvm_ir_text(gir_mod, target_opts, opt_opts, artifact)};
        if (!ir) {
            return clap::fatal_error(error_stream,
                                     ir.error().get_message().value_or(GHOTI_UNKNOWN_ERROR),
                                     clap::error::COMPILATION_FAILED);
        }
        TRY(write_file(*emit_llvm_ir_path, *ir, "LLVM IR"));
    }

    if (emit_asm_path) {
        auto asm_text{analyzer.emit_asm_text(gir_mod, target_opts, opt_opts, artifact)};
        if (!asm_text) {
            return clap::fatal_error(error_stream,
                                     asm_text.error().get_message().value_or(GHOTI_UNKNOWN_ERROR),
                                     clap::error::COMPILATION_FAILED);
        }
        TRY(write_file(*emit_asm_path, *asm_text, "assembly"));
    }

    return {};
}

} // namespace ghoti::cmd::build
