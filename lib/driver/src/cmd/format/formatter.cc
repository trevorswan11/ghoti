#include "driver/cmd/format/formatter.hh"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdx/assert.hh>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/dumper.hh"
#include "compiler/ast/formatter.hh"
#include "compiler/syntax/parser.hh"
#include "driver/clap/error.hh"
#include "driver/cmd/format/options.hh"
#include "support/string_utils.hh"

namespace ghoti::cmd {

auto formatter::execute() -> stdx::result<void, clap::error> {
    if (opts_.reading_stdin) {
        auto source_code{string_utils::read_stream(std::cin)};
        if (std::cin.bad()) {
            return clap::fatal_error(
                error_stream_, "failed to read from stdin", clap::error::STDIN_LOAD_FAILED);
        }

        const auto display_name{
            opts_.stdin_filepath.transform([](const auto& p) { return p.string(); })};
        if (!process_target(source_code, display_name, nullptr)) {
            return stdx::err{clap::error::FORMATTING_FAILED};
        }
        return {};
    }

    bool success{true};
    for (const auto& path : opts_.input_paths) {
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            clap::warn_error(error_stream_, fmt::format("could not open file '{}'", path.string()));
            success = false;
            continue;
        }

        auto source_code{string_utils::read_stream(file)};
        if (!process_target(source_code, path.string(), &path)) { success = false; }
    }

    if (!success) { return stdx::err{clap::error::FORMATTING_FAILED}; }
    return {};
}

auto formatter::process_target(std::string_view                           source_code,
                               const stdx::option<std::string>&           display_name,
                               stdx::option<const std::filesystem::path&> file_path) -> bool {
    syntax::parser parser{source_code};
    ast::AST       ast;
    const auto     diagnostics{parser.consume(ast)};

    if (!diagnostics.empty()) {
        for (const auto& diag : diagnostics) {
            fmt::println(error_stream_,
                         "{}",
                         diag.to_string(display_name, diagnostics.get_terminal_status()));
        }
        return false;
    }

    std::ostringstream formatted_os;
    ast::formatter ast_fmt{ast, formatted_os, opts_.max_width, opts_.indent_spaces, source_code};
    ast_fmt.format();
    auto formatted_code{formatted_os.view()};

    // Verify ASTs for each are valid before writing out the result
    if (!ast::dumper::compare_source_asts(source_code, formatted_code)) {
        clap::warn_error(
            error_stream_,
            display_name
                ? fmt::format("file '{}' was not formatted due to an unknown error", *display_name)
                : "stdin was not formatted due to an unknown error");
        return false;
    }

    const bool is_different{source_code != formatted_code};
    if (opts_.check_only) {
        if (is_different) {
            clap::warn_error(error_stream_,
                             display_name ? fmt::format("file '{}' is not formatted", *display_name)
                                          : "stdin is not formatted");
        }
        return !is_different;
    }

    if (opts_.write_in_place) {
        if (is_different && file_path) {
            std::ofstream file{*file_path, std::ios::binary | std::ios::trunc};
            if (!file) {
                clap::warn_error(
                    error_stream_,
                    fmt::format("could not open file '{}' for writing", file_path->string()));
                return false;
            }
            fmt::print(file, "{}", formatted_code);
            if (!file) {
                clap::warn_error(error_stream_,
                                 fmt::format("failed to write to file '{}'", file_path->string()));
                return false;
            }
        }
        return true;
    }

    fmt::print(out_stream_, "{}", formatted_code);
    return true;
}

} // namespace ghoti::cmd
