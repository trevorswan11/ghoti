#include "driver/cmd/repl.hh"

#include <filesystem>
#include <iostream>
#include <string>

#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <magic_enum/magic_enum.hpp>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "compiler/ast/dumper.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/analyzer.hh"
#include "driver/clap/error.hh"

namespace ghoti::cmd {

auto repl::execute() -> stdx::result<void, clap::error> {
    PROFILE_FUNCTION();
    const std::filesystem::path stdin_path = "stdin.gh";
    while (true) {
        fmt::print(">>> ");
        line_.clear();

        if (!std::getline(std::cin, line_)) { break; }
        auto trimmed{stdx::string::trim(line_)};
        if (trimmed == "exit") { break; }
        if (trimmed.empty()) { continue; }

        mod::memory_loader  loader;
        mod::module_manager manager{loader};
        loader.add(stdin_path, std::string{trimmed});

        sema::analyzer analyzer{manager, error_stream_, true};
        if (!analyzer.analyze(stdin_path)) {
            return clap::fatal_error(
                error_stream_, "failed to load input from stdin", clap::error::STDIN_LOAD_FAILED);
        }

        // Print debug information from each stage
        const auto stdin_mod{*manager.try_get_file_module(stdin_path)};
        if (stdin_mod->is_errored()) { continue; }

        ast::dumper dumper{stdin_mod->ast, std::cout};
        for (const auto& node : stdin_mod->ast) { dumper.dump(node); }
        if (stdin_mod->is_poisoned()) { continue; }

        const auto& registry{analyzer.get_registry()};
        fmt::println("{} total tables, {} top-level symbols collected",
                     analyzer.get_registry().size(),
                     analyzer.get_table(*stdin_mod->root_table_idx).size());

        for (usize i{0}; const auto& table : registry) {
            if (analyzer.get_prelude_index_opt() == i) { continue; }
            fmt::println("Symbols in table idx {}:", i);
            for (const auto& [name, proxy] : table) {
                fmt::println("  - Symbol '{}': status = {}",
                             name,
                             magic_enum::enum_name(proxy.symbol.get_status()));
            }
            i += 1;
        }
    }

    return {};
}

} // namespace ghoti::cmd
