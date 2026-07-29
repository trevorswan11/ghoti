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

namespace CLI { class App; } // namespace CLI

namespace ghoti::cmd {

struct module_binding {
    std::string           name;
    std::filesystem::path path;
};

// Raw options populated directly by CLI parser
struct build_options_raw {
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
};

struct build_options_base {
    std::filesystem::path              input_path{};
    std::filesystem::path              output_path{};
    codegen::target_options            target_opts{};
    codegen::optimizer_options         opt_opts{};
    std::vector<module_binding>        modules{};
    std::vector<std::filesystem::path> extra_objects{};
    std::vector<std::filesystem::path> library_paths{};
    std::vector<std::string>           libraries{};

    static auto process_raw(const build_options_raw& raw,
                            std::string_view         default_ext,
                            std::ostream&            error_stream)
        -> stdx::result<build_options_base, clap::error>;

    // Converts the input path from absolute to relative if needed
    auto normalize_input_path() -> void;

    [[nodiscard]] auto setup_module_manager(mod::module_manager& manager,
                                            std::ostream&        error_stream)
        -> stdx::result<void, clap::error>;

    auto analyze(sema::analyzer& analyzer, mod::module_manager& manager, std::ostream& error_stream)
        -> stdx::result<gsl::not_null<ghoti::mod::module*>, clap::error>;
};

struct build_obj_opts : public build_options_raw {};
struct build_exe_opts : public build_options_raw {};
struct build_lib_opts : public build_options_raw {};

// Helper to register standard build options into CLI subcommands
auto setup_build_options_flags(CLI::App*          subcmd,
                               build_options_raw& opts,
                               std::string_view   output_desc,
                               bool omit_lib_linking = false) -> void;

} // namespace ghoti::cmd
