#include "compiler/module/module.hh"

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/module/error.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/syntax/parser.hh"
#include "support/diagnostic.hh"
#include "support/source_file.hh"
#include "support/style.hh"

namespace ghoti::mod {

auto format_module_diagnostic(std::ostream&                         os, // NOLINT
                              const detail::formattable_diagnostic& diag,
                              stdx::option<const mod::module&>      module,
                              stdx::option<bool>                    in_terminal) -> std::ostream& {
    const auto tty{in_terminal.value_or(stdx::is_tty())};

    // Without a module, there is no source path and formatting is done trivially
    if (!module) { return format_diagnostic(os, diag, stdx::none, tty); }

    // Without location, there's no way to point to an error
    format_diagnostic(os, diag, module->path.string(), tty);
    if (!diag.location) { return os; }

    // Diagnostic error messages can include the location
    const auto [line, caret]{module->source.get_diagnostic_strings(*diag.location)};
    fmt::print(os, "\n    {}", line);
    if (caret) { os << fmt::format(tty ? style::GREEN : style::BASE, "\n    {}", *caret); }
    return os;
}

auto module::print_diagnostics(std::ostream& os) const -> void {
    PROFILE_FUNCTION();
    if (is_ok()) { return; }
    diagnostics.visit(
        [this, &os](const auto& list) -> void {
            for (const auto& diag : list) {
                format_module_diagnostic(
                    os, diag.to_formattable(), *this, list.get_terminal_status())
                    << "\n";
            }
        },
        [](stdx::monostate) -> void {
            UNREACHABLE("This function should've never been called with stdx::monostate");
        });
}

auto module_manager::try_get_file_module(const std::filesystem::path& path,
                                         const std::filesystem::path& parent_path)
    -> stdx::result<gsl::not_null<module*>, diagnostic> {
    PROFILE_FUNCTION();
    ASSERT((parent_path.empty() || parent_path.is_absolute()) &&
           "Parent path must be absolute or empty");
    if (!path.is_relative()) {
        return make_mod_err(fmt::format("Requested file '{}' is absolute", path.string()),
                            error::MODULE_PATH_NOT_RELATIVE);
    };

    const auto normalized{loader_.normalize(parent_path.empty() ? path : parent_path / path)};
    if (!normalized) { return make_mod_err(normalized.error()); }
    return try_get(*normalized);
}

auto module_manager::try_get_library_module(std::string_view name)
    -> stdx::result<gsl::not_null<module*>, diagnostic> {
    PROFILE_FUNCTION();
    auto it{module_lut_.find(name)};
    if (it == module_lut_.end()) {
        return make_mod_err(fmt::format("Unknown module '{}'", name), error::MODULE_DOES_NOT_EXIST);
    }
    return try_get(it->second);
}

auto module_manager::add_library_module(std::string_view name, const std::filesystem::path& path)
    -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    const auto normalized{loader_.normalize(path)};
    if (!normalized) { return make_mod_err(normalized.error()); }

    if (auto it{module_lut_.find(name)}; it != module_lut_.end()) {
        if (it->second == normalized) { return {}; }
        return make_mod_err(fmt::format("Attempt to add duplicate module '{}' from path '{}' "
                                        "which already exists at path '{}'",
                                        name,
                                        path.string(),
                                        it->second.string()),
                            error::MODULE_ALREADY_EXISTS);
    }

    module_lut_.emplace(name, *normalized);
    return {};
}

auto module_manager::print_all_diagnostics(std::ostream& os) const -> void {
    for (const auto& [path, mod] : modules_) {
        if (mod->is_poisoned() || mod->is_errored()) { mod->print_diagnostics(os); }
    }
}

auto module_manager::try_get(const std::filesystem::path& path)
    -> stdx::result<gsl::not_null<module*>, diagnostic> {
    // Prevent re-parsing by checking the map, safe as pointers are stable
    PROFILE_FUNCTION();
    if (auto it{modules_.find(path)}; it != modules_.end()) { return it->second.get(); }
    auto source{TRY(loader_.load(path))};

    auto mod{stdx::make_box<module>(path, path.parent_path(), source_file{std::move(source)})};
    syntax::parser p{mod->source};
    auto           diagnostics{p.consume(mod->ast)};

    mod->sema_side_tables.resize(mod->ast.get_pool_sizes());
    mod->parse_diagnostics.reserve(diagnostics.size());
    for (const auto& d : diagnostics) { mod->parse_diagnostics.push_back(d.snapshot()); }
    mod->state       = diagnostics.empty() ? module_state::PARSED : module_state::ERRORED;
    mod->diagnostics = std::move(diagnostics);

    auto* ptr = mod.get();
    modules_.emplace(path, std::move(mod));
    return ptr;
}

auto module_manager::get_or_create_builtin_module(std::string_view source) -> module& {
    PROFILE_FUNCTION();
    if (builtin_module_) { return *builtin_module_; }

    builtin_module_ = stdx::make_box<module>(std::filesystem::path{"<builtin>"},
                                             std::filesystem::path{},
                                             source_file{std::string{source}});
    syntax::parser p{builtin_module_->source};
    const auto     diagnostics{p.consume(builtin_module_->ast)};
    VERIFY(diagnostics.empty(), "the compiler-provided target-fact enum source must parse cleanly");

    builtin_module_->sema_side_tables.resize(builtin_module_->ast.get_pool_sizes());
    builtin_module_->state = module_state::PARSED;
    return *builtin_module_;
}

} // namespace ghoti::mod
