#pragma once

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"

namespace CLI { class App; }           // namespace CLI
namespace ghoti::gir { class module; } // namespace ghoti::gir

namespace ghoti::cmd::build {

struct module_binding {
    std::string           name;
    std::filesystem::path path;
};

// Raw options populated directly by CLI parser
struct raw_options {
    std::string              input;
    std::string              output;
    std::string              target;
    std::string              cpu{"generic"};
    std::string              features;
    std::string              opt_level_str;
    std::vector<std::string> module_raw_args;
    std::vector<std::string> extra_objects;
    std::vector<std::string> library_paths;
    std::vector<std::string> libraries;
    bool                     release{false};
    bool                     debug_passes{false};
    bool                     time_passes{false};
    bool                     dynamic{false};
    bool                     unsafe{false};

    std::string emit_gir_path;
    std::string emit_llvm_ir_path;
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
    bool                               dynamic{false};
    bool                               runtime_safety{true};

    stdx::option<std::filesystem::path> emit_gir_path{};
    stdx::option<std::filesystem::path> emit_llvm_ir_path{};

    static auto process_raw(const raw_options&   raw,
                            codegen::output_type type,
                            std::ostream& error_stream) -> stdx::result<options, clap::error>;

    // Writes the GIR / LLVM IR dumps requested via --emit-gir / --emit-llvm-ir, if any.
    [[nodiscard]] auto emit_debug_artifacts(sema::analyzer& analyzer,
                                            gir::module&    gir_mod,
                                            std::ostream&   error_stream) const
        -> stdx::result<void, clap::error>;

    // Converts the input path from absolute to relative if needed
    auto make_path_relative() -> void;

    [[nodiscard]] auto setup_module_manager(mod::module_manager& manager,
                                            std::ostream&        error_stream)
        -> stdx::result<void, clap::error>;

    auto analyze(sema::analyzer& analyzer, mod::module_manager& manager, std::ostream& error_stream)
        -> stdx::result<gsl::not_null<ghoti::mod::module*>, clap::error>;
};

// Helper to register standard build options into CLI subcommands
auto setup_flags(CLI::App* subcmd, raw_options& opts, stdx::option<std::string_view> output_desc)
    -> void;

} // namespace ghoti::cmd::build
