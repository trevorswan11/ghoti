#pragma once

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"

namespace CLI { class App; } // namespace CLI

namespace ghoti::cmd::build {

struct module_binding {
    std::string           name;
    std::filesystem::path path;
};

// Raw options populated directly by CLI parser
struct raw_options {
    std::string                input;
    std::string                output;
    std::string                target;
    std::string                cpu{"generic"};
    std::string                features;
    std::optional<std::string> opt_level_str; // unset unless `-O` was given (even as `-O ""`)
    std::vector<std::string>   module_raw_args;
    std::vector<std::string>   extra_objects;
    std::vector<std::string>   library_paths;
    std::vector<std::string>   libraries;
    std::vector<std::string>   forwarded_args;
    bool                       release{false};
    bool                       debug_passes{false};
    bool                       time_passes{false};
    bool                       dynamic{false};
    bool                       unsafe{false};

    std::string emit_gir_path;
    std::string emit_llvm_ir_path;
    std::string emit_asm_path;
};

struct options {
    std::filesystem::path              input_path{};
    std::filesystem::path              output_path{};
    codegen::target_options            target_opts{};
    codegen::optimizer_options         opt_opts{};
    std::vector<module_binding>        modules{};
    std::vector<std::filesystem::path> extra_objects{};
    std::vector<std::filesystem::path> library_paths{};
    std::vector<std::string>           libraries{};
    std::vector<std::string>           forwarded_args{};
    bool                               dynamic{false};
    bool                               runtime_safety{true};
    bool output_explicit{false}; // `ghoti test` only: true when `-o/--output` was given

    stdx::option<std::filesystem::path> emit_gir_path{};
    stdx::option<std::filesystem::path> emit_llvm_ir_path{};
    stdx::option<std::filesystem::path> emit_asm_path{};

    static auto process_raw(const raw_options&   raw,
                            codegen::output_type type,
                            std::ostream& error_stream) -> stdx::result<options, clap::error>;

    // Writes the GIR / LLVM IR / assembly dumps requested via
    // --emit-gir / --emit-llvm-ir / --emit-asm, if any.
    [[nodiscard]] auto emit_debug_artifacts(sema::analyzer&      analyzer,
                                            gir::module&         gir_mod,
                                            std::ostream&        error_stream,
                                            sema::build_artifact artifact) const
        -> stdx::result<void, clap::error>;

    // Points `output_path` at a fresh absolute temp executable path tagged with `tag`
    auto use_temp_executable_output(std::string_view tag) -> void;

    auto make_output_path_absolute() -> void;
};

using analyzed_module = std::pair<gsl::not_null<mod::module*>, gir::module>;

// Owns the loader, module manager, and analyzer every compiling subcommand drives.
class compilation {
  public:
    compilation(options& opts, std::ostream& error_stream);

    // Validates the input file, registers the stdlib and `-m` modules, and runs sema.
    [[nodiscard]] auto analyze(bool for_test_executable = false)
        -> stdx::result<analyzed_module, clap::error>;

    [[nodiscard]] auto get_analyzer() noexcept -> sema::analyzer& { return analyzer_; }

  private:
    [[nodiscard]] auto validate_input_path() -> stdx::result<void, clap::error>;
    [[nodiscard]] auto ensure_output_directory() -> stdx::result<void, clap::error>;
    [[nodiscard]] auto setup_module_manager() -> stdx::result<void, clap::error>;

  private:
    options&            opts_;
    std::ostream&       error_stream_;
    mod::file_loader    loader_;
    mod::module_manager manager_;
    sema::analyzer      analyzer_;
};

[[nodiscard]] auto report_codegen_error(std::ostream& error_stream, const codegen::diagnostic& diag)
    -> stdx::err<clap::error>;

// Runs a just-built executable and maps its exit status onto the driver's result.
[[nodiscard]] auto run_built_executable(const options& opts,
                                        std::ostream&  error_stream,
                                        bool cleanup_output) -> stdx::result<void, clap::error>;

// Helper to register standard build options into CLI subcommands
auto setup_flags(CLI::App* subcmd, raw_options& opts, stdx::option<std::string_view> output_desc)
    -> void;

} // namespace ghoti::cmd::build
