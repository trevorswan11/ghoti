#include "compiler/module/module.hh"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/module/error.hh"
#include "compiler/module/parse_cache.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "support/diagnostic.hh"
#include "support/diagnostic_renderer.hh"
#include "support/path_utils.hh"
#include "support/source_file.hh"
#include "support/style.hh"

namespace ghoti::mod {

namespace {

[[nodiscard]] auto render_source_of(const module_t& mod) -> render_source {
    return {mod.path, path_utils::display(mod.path), &mod.source};
}

auto render_printable(std::ostream&               os,
                      const printable_diagnostic& diag,
                      const source_resolver&      resolve) -> void {
    render_options options{.color = diag.in_terminal.value_or(stdx::is_tty())};
    render_diagnostic(os, diag.view, render_source_of(*diag.owner), resolve, options);
}

[[nodiscard]] auto counts_toward_limit(const printable_diagnostic& diag) -> bool {
    return diag.view.level.value_or(diagnostic_level::ERROR) == diagnostic_level::ERROR;
}

} // namespace

auto sort_diagnostics(std::vector<printable_diagnostic>& diagnostics) -> void {
    const auto key{[](const printable_diagnostic& d) {
        return std::tuple{path_utils::display(d.owner->path),
                          d.view.location(),
                          d.view.error_name,
                          d.view.message};
    }};
    std::ranges::stable_sort(diagnostics, [&key](const auto& a, const auto& b) {
        const auto [a_path, a_at, a_code, a_message]{key(a)};
        const auto [b_path, b_at, b_code, b_message]{key(b)};
        if (a_path != b_path) { return a_path < b_path; }
        if (a_at != b_at) { return !a_at || (b_at && *a_at < *b_at); }
        if (a_code != b_code) { return a_code < b_code; }
        return a_message < b_message;
    });
}

auto module_t::source_of(ast::node_id id) const -> std::string_view {
    const std::string_view text{source};
    const auto             offset_of{[&](const source_location& loc) -> usize {
        usize offset{0};
        for (usize line{0}; line < loc.line && offset < text.size(); ++line) {
            const auto newline{text.find('\n', offset)};
            if (newline == std::string_view::npos) { return text.size(); }
            offset = newline + 1;
        }
        return std::min(offset + loc.column, text.size());
    }};
    // `end_location_of` ends just past the node's last token
    const auto start{offset_of(ast.location_of(id))};
    const auto end{offset_of(ast.end_location_of(id))};
    return start < end ? stdx::string::substr(text, start, end - start) : std::string_view{};
}

auto module_t::printable_diagnostics() const -> std::vector<printable_diagnostic> {
    std::vector<printable_diagnostic> out;
    diagnostics.visit([](const stdx::monostate&) -> void {},
                      [this, &out](const auto& list) -> void {
                          for (const auto& diag : list) {
                              const auto& owner{diag.origin() ? *diag.origin() : *this};
                              // The module the error points into may already report it itself
                              if (&owner != this && owner.reports(diag)) { continue; }
                              out.push_back(
                                  {&owner, diag.to_formattable(), list.get_terminal_status()});
                          }
                      });
    return out;
}

auto module_t::printable_warnings() const -> std::vector<printable_diagnostic> {
    std::vector<printable_diagnostic> out;
    for (const auto& warning : warnings) {
        out.push_back({this, warning.to_formattable(), warnings.get_terminal_status()});
    }
    return out;
}

auto module_t::print_diagnostics(std::ostream& os) const -> void {
    PROFILE_FUNCTION();
    if (is_ok()) { return; }
    auto pending{printable_diagnostics()};
    sort_diagnostics(pending);
    for (const auto& diag : pending) {
        render_printable(os, diag, {});
        os << "\n";
    }
}

auto module_manager::try_get_file_module(const std::filesystem::path& path,
                                         const std::filesystem::path& parent_path)
    -> stdx::result<gsl::not_null<module_t*>, diagnostic> {
    PROFILE_FUNCTION();
    ASSERT((parent_path.empty() || parent_path.is_absolute()) &&
           "Parent path must be absolute or empty");
    // An absolute path names its file directly; a relative one is resolved from the importer
    const auto requested{parent_path.empty() || path.is_absolute() ? path : parent_path / path};
    const auto normalized{loader_.normalize(requested)};
    if (!normalized) { return make_mod_err(normalized.error()); }
    return try_get(*normalized);
}

auto module_manager::try_get_library_module(std::string_view name)
    -> stdx::result<gsl::not_null<module_t*>, diagnostic> {
    PROFILE_FUNCTION();
    auto it{module_lut_.find(name)};
    if (it == module_lut_.end()) {
        return make_mod_err(fmt::format("Unknown module '{}'", name), error::MODULE_DOES_NOT_EXIST);
    }
    return try_get(it->second);
}

auto module_manager::find_file_module(const std::filesystem::path& path)
    -> stdx::option<module_t&> {
    const auto normalized{loader_.normalize(path)};
    if (!normalized) { return stdx::none; }
    const auto loaded{modules_.find(*normalized)};
    if (loaded == modules_.end()) { return stdx::none; }
    return *loaded->second;
}

auto module_manager::find_loaded_library_module(std::string_view name) -> stdx::option<module_t&> {
    const auto path{module_lut_.find(name)};
    if (path == module_lut_.end()) { return stdx::none; }
    const auto loaded{modules_.find(path->second)};
    if (loaded == modules_.end()) { return stdx::none; }
    return *loaded->second;
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

auto module_t::print_warnings(std::ostream& os) const -> void {
    auto pending{printable_warnings()};
    sort_diagnostics(pending);
    for (const auto& warning : pending) {
        render_printable(os, warning, {});
        os << "\n";
    }
}

auto module_manager::find_loaded(const std::filesystem::path& path) const
    -> stdx::option<const module_t&> {
    if (const auto it{modules_.find(path)}; it != modules_.end()) { return *it->second; }
    for (const auto* special : {builtin_module_.get(), start_module_.get()}) {
        if (special && special->path == path) { return *special; }
    }
    return stdx::none;
}

auto module_manager::print_all_warnings(std::ostream& os) const -> void {
    std::vector<printable_diagnostic> pending;
    for (const auto& [path, mod] : modules_) {
        auto warnings{mod->printable_warnings()};
        pending.insert(pending.end(), warnings.begin(), warnings.end());
    }
    sort_diagnostics(pending);
    const source_resolver resolve{[this](const std::filesystem::path& path) {
        return find_loaded(path).transform(render_source_of);
    }};
    for (const auto& warning : pending) {
        render_printable(os, warning, resolve);
        os << "\n";
    }
}

auto module_manager::take_unprinted(const module_t& mod) const
    -> std::vector<printable_diagnostic> {
    if (!mod.is_poisoned() && !mod.is_errored()) { return {}; }
    if (!printed_diagnostics_.insert(&mod).second) { return {}; }
    return mod.printable_diagnostics();
}

auto module_manager::print_limited(std::vector<printable_diagnostic>&& pending,
                                   std::ostream&                       os) const -> void {
    sort_diagnostics(pending);
    const source_resolver resolve{[this](const std::filesystem::path& path) {
        return find_loaded(path).transform(render_source_of);
    }};

    usize hidden{0};
    for (const auto& diag : pending) {
        if (counts_toward_limit(diag)) {
            if (error_limit_ && errors_printed_ >= *error_limit_) {
                hidden += 1;
                continue;
            }
            errors_printed_ += 1;
        }
        render_printable(os, diag, resolve);
        os << "\n";
    }
    if (hidden > 0) {
        fmt::println(os, "{} more error{} not shown", hidden, hidden == 1 ? "" : "s");
    }
}

auto module_manager::print_module_diagnostics(const module_t& mod, std::ostream& os) const -> void {
    print_limited(take_unprinted(mod), os);
}

auto module_manager::print_all_diagnostics(std::ostream& os) const -> void {
    std::vector<printable_diagnostic> pending;
    const auto                        take{[&](const module_t& mod) {
        auto taken{take_unprinted(mod)};
        pending.insert(pending.end(), taken.begin(), taken.end());
    }};
    for (const auto& [path, mod] : modules_) { take(*mod); }
    // An error inside an emitted `builtin` declaration is attributed to `builtin` itself
    if (builtin_module_) { take(*builtin_module_); }
    if (start_module_) { take(*start_module_); }
    print_limited(std::move(pending), os);
}

auto module_manager::any_errored() const noexcept -> bool {
    // This is necessary as the module itself is lazily checked
    for (const auto& [path, mod] : modules_) {
        if (mod->is_poisoned() || mod->is_errored()) { return true; }
    }
    if (start_module_ && (start_module_->is_poisoned() || start_module_->is_errored())) {
        return true;
    }
    return builtin_module_ && (builtin_module_->is_poisoned() || builtin_module_->is_errored());
}

auto module_manager::try_get(const std::filesystem::path& path)
    -> stdx::result<gsl::not_null<module_t*>, diagnostic> {
    // Prevent re-parsing by checking the map, safe as pointers are stable
    PROFILE_FUNCTION();
    if (auto it{modules_.find(path)}; it != modules_.end()) { return it->second.get(); }
    auto source{TRY(loader_.load(path))};

    stdx::box<module_t> mod;
    syntax::diagnostics diagnostics;
    if (parse_cache_) {
        auto parsed{parse_cache_->get(path, std::move(source))};
        mod = stdx::make_box<module_t>(
            path, path.parent_path(), source_file{std::string_view{parsed->source}});
        mod->ast    = parsed->ast.borrow(*arena_);
        diagnostics = parsed->diagnostics.create_new();
        for (const auto& d : parsed->diagnostics) { diagnostics.push_back(d); }
        mod->parsed_from = std::move(parsed);
    } else {
        mod = stdx::make_box<module_t>(path, path.parent_path(), source_file{std::move(source)});
        diagnostics = parse_source(mod->source, mod->ast, *arena_);
    }

    mod->sema_side_tables.resize(mod->ast.get_pool_sizes());
    mod->parse_diagnostics.reserve(diagnostics.size());
    for (const auto& d : diagnostics) { mod->parse_diagnostics.emplace_back(d.snapshot()); }
    mod->state       = diagnostics.empty() ? module_state::PARSED : module_state::ERRORED;
    mod->diagnostics = std::move(diagnostics);
    mod->is_start    = is_start_path(path);

    const gsl::not_null<module_t*> ptr{mod.get()};
    modules_.emplace(path, std::move(mod));
    return ptr;
}

auto module_manager::get_or_create_builtin_module(std::string_view source) -> module_t& {
    PROFILE_FUNCTION();
    if (builtin_module_) { return *builtin_module_; }

    builtin_module_ = stdx::make_box<module_t>(std::filesystem::path{"<builtin>"},
                                               std::filesystem::path{},
                                               source_file{std::string{source}});
    syntax::parser p{builtin_module_->source};
    p.enable_default_wrappers();
    const auto diagnostics{p.consume(builtin_module_->ast, *arena_)};
    VERIFY(diagnostics.empty(), "the compiler-provided target-fact enum source must parse cleanly");

    builtin_module_->sema_side_tables.resize(builtin_module_->ast.get_pool_sizes());
    builtin_module_->state = module_state::PARSED;
    return *builtin_module_;
}

auto module_manager::is_start_path(const std::filesystem::path& path) -> bool {
    if (!start_path_) { start_path_.emplace(find_start()); }
    if (!*start_path_) { return false; }
    std::error_code ec;
    return std::filesystem::equivalent(path, **start_path_, ec) && !ec;
}

auto module_manager::get_or_create_start_module(const std::filesystem::path& path,
                                                std::string_view             source) -> module_t& {
    PROFILE_FUNCTION();
    if (start_module_) { return *start_module_; }

    start_module_ =
        stdx::make_box<module_t>(path, path.parent_path(), source_file{std::string{source}});
    syntax::parser p{start_module_->source};
    p.enable_default_wrappers();
    auto diagnostics{p.consume(start_module_->ast, *arena_)};

    start_module_->sema_side_tables.resize(start_module_->ast.get_pool_sizes());
    start_module_->parse_diagnostics.reserve(diagnostics.size());
    for (const auto& d : diagnostics) {
        start_module_->parse_diagnostics.emplace_back(d.snapshot());
    }
    start_module_->state       = diagnostics.empty() ? module_state::PARSED : module_state::ERRORED;
    start_module_->diagnostics = std::move(diagnostics);
    start_module_->is_start    = true;
    return *start_module_;
}

auto module_t::get_overlay_node_type(usize idx) const noexcept
    -> stdx::option<stdx::option<sema::type&>> {
    if (!active_body_diff) { return stdx::none; }
    return active_body_diff->find_node_type(idx);
}

auto module_t::get_overlay_explicit_type(usize idx) const noexcept
    -> stdx::option<stdx::option<sema::type&>> {
    if (!active_body_diff) { return stdx::none; }
    return active_body_diff->find_explicit_type(idx);
}

auto module_t::record_node_write(usize idx) noexcept -> void {
    if (active_write_log) { active_write_log->node_idxs.emplace_back(idx); }
}

auto module_t::record_explicit_write(usize idx) noexcept -> void {
    if (active_write_log) { active_write_log->explicit_idxs.emplace_back(idx); }
}

auto module_t::record_call_write(usize idx) noexcept -> void {
    if (active_write_log) { active_write_log->call_idxs.emplace_back(idx); }
}

auto module_t::attributes_of(ast::node_id id) const noexcept
    -> stdx::option<const sema::resolved_attributes&> {
    if (active_body_diff) {
        if (const auto resolved{active_body_diff->find_attributes(id.get_index())}) {
            return resolved;
        }
    }
    const auto it{node_attributes.find(id.get_index())};
    if (it == node_attributes.end()) { return stdx::none; }
    return it->second;
}

auto module_t::set_node_attributes(ast::node_id id, const sema::resolved_attributes& resolved)
    -> void {
    node_attributes.insert_or_assign(id.get_index(), resolved);
    if (active_write_log) { active_write_log->attribute_idxs.emplace_back(id.get_index()); }
}

auto module_t::get_overlay_call_target(usize idx) const noexcept
    -> stdx::option<stdx::option<std::string_view>> {
    if (!active_body_diff) { return stdx::none; }
    return active_body_diff->find_call_target(idx);
}

auto module_t::get_if_branch_opt(usize node_idx) const noexcept -> stdx::option<if_branch> {
    if (active_body_diff) {
        if (const auto br{active_body_diff->find_if_branch(node_idx)}) { return br; }
    }
    if (const auto it{if_comptime_results.find(node_idx)}; it != if_comptime_results.end()) {
        return it->second;
    }
    return stdx::none;
}

auto module_t::get_match_arm_opt(usize node_idx) const noexcept -> stdx::opt_size {
    if (active_body_diff) {
        if (const auto arm{active_body_diff->find_match_arm(node_idx)}) { return arm; }
    }
    if (const auto it{match_arm_results.find(node_idx)}; it != match_arm_results.end()) {
        return stdx::opt_size{it->second};
    }
    return stdx::none;
}

} // namespace ghoti::mod
