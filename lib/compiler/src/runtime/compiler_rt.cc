#include "compiler/runtime/compiler_rt.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/target.hh"
#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/context.hh"
#include "support/env.hh"
#include "support/tempfile.hh"

namespace ghoti::runtime {

namespace {

// One archive per distinct code-generation setup; `error` is kept so a broken compiler_rt is
// reported on every link rather than only the first
struct archive_entry {
    stdx::option<std::filesystem::path> archive{};
    stdx::option<codegen::diagnostic>   error{};
};

struct archive_memo {
    std::mutex                                        mutex;
    stdx::nullable_box<tempdir>                       dir;
    std::map<std::string, archive_entry, std::less<>> entries;
    usize                                             builds{0};
};

[[nodiscard]] auto memo() -> archive_memo& {
    static archive_memo instance;
    return instance;
}

[[nodiscard]] auto memo_key(const codegen::target_options& target_opts,
                            const llvm::Triple&            triple,
                            const std::filesystem::path&   root) -> std::string {
    const auto reloc{target_opts.reloc ? static_cast<i32>(*target_opts.reloc) : -1};
    const auto code{target_opts.code ? static_cast<i32>(*target_opts.code) : -1};
    return fmt::format("{}|{}|{}|{}|{}|{}",
                       triple.str(),
                       target_opts.cpu,
                       target_opts.features,
                       reloc,
                       code,
                       root.string());
}

[[nodiscard]] auto build_failed(const llvm::Triple& triple, std::string_view details)
    -> codegen::diagnostic {
    return codegen::diagnostic{
        fmt::format("the compiler builtins failed to build for '{}':\n{}", triple.str(), details),
        codegen::error::COMPILER_RT_BUILD_FAILED};
}

[[nodiscard]] auto is_identifier_char(char c) -> bool {
    return std::isalnum(static_cast<u8>(c)) != 0 || c == '_' || c == '$' || c == '.';
}

// Local labels (`.LBB0_1`, Mach-O `Ltmp0`, `$` temporaries) don't start a new function
[[nodiscard]] auto is_local_label(std::string_view label) -> bool {
    return label.starts_with('.') || label.starts_with('L') || label.starts_with('$');
}

[[nodiscard]] auto build_archive(const std::filesystem::path&   root,
                                 const codegen::target_options& target_opts,
                                 const llvm::Triple&            triple,
                                 const std::filesystem::path&   archive)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    std::ostringstream  diagnostics;
    mod::file_loader    loader;
    mod::module_manager manager{loader};

    // Builtins are always optimized and never panic, whatever the program being linked asks for
    auto rt_target{target_opts};
    rt_target.level = codegen::opt_level::O2;
    sema::analyzer analyzer{manager, diagnostics, false, rt_target, false, false};
    analyzer.set_optimize_mode(sema::optimize_mode::RELEASE_FAST);

    // The analyzer already reported the root module's own errors, as the driver relies on
    auto       gir_mod{analyzer.analyze(root)};
    const auto module{manager.try_get_file_module(root)};
    if (!gir_mod) {
        if (const auto message{gir_mod.error().get_message()}) { diagnostics << *message << '\n'; }
        return stdx::err{build_failed(triple, diagnostics.str())};
    }
    if (!module || (*module)->is_poisoned() || (*module)->is_errored()) {
        return stdx::err{build_failed(triple, diagnostics.str())};
    }

    // A module compiler_rt imports is parsed lazily and may fail on its own
    if (manager.any_errored()) {
        manager.print_all_diagnostics(diagnostics);
        return stdx::err{build_failed(triple, diagnostics.str())};
    }

    const codegen::optimizer_options opt_opts{.level = codegen::opt_level::O2};
    const auto                       asm_text{
        TRY(analyzer.emit_asm_text(*gir_mod, rt_target, opt_opts, sema::build_artifact::LIBRARY))};
    if (const auto self_calls{find_self_calling_libcalls(asm_text, rt_target)};
        !self_calls.empty()) {
        std::string details;
        for (const auto& name : self_calls) {
            details += fmt::format(
                "`{0}` is compiled into a call to `{0}`; implement it without the operation it "
                "provides\n",
                name);
        }
        return stdx::err{codegen::diagnostic{
            fmt::format(
                "the compiler builtins for '{}' call themselves:\n{}", triple.str(), details),
            codegen::error::COMPILER_RT_BUILD_FAILED}};
    }
    return analyzer.emit_static_library(*gir_mod, rt_target, opt_opts, archive);
}

} // namespace

auto locate_compiler_rt(const codegen::compiler_rt_options& options)
    -> stdx::option<std::filesystem::path> {
    std::error_code ec;
    if (options.root) {
        if (std::filesystem::is_regular_file(*options.root, ec)) { return *options.root; }
        return stdx::none;
    }
    if (const auto env{get_env("GHOTI_COMPILER_RT")}) {
        std::filesystem::path path{*env};
        if (std::filesystem::is_regular_file(path, ec)) { return path; }
        return stdx::none;
    }
    return mod::find_lib_path("compiler_rt/compiler_rt.gh");
}

auto compiler_rt_imports(const std::filesystem::path&   object,
                         const codegen::target_options& target_opts)
    -> stdx::result<std::vector<std::string>, codegen::diagnostic> {
    const auto triple{codegen::resolve_target_triple(target_opts.triple_str)};
    auto       binary{llvm::object::ObjectFile::createObjectFile(object.string())};
    if (!binary) {
        return codegen::make_codegen_err(fmt::format("failed to read object '{}': {}",
                                                     object.string(),
                                                     llvm::toString(binary.takeError())),
                                         codegen::error::OBJECT_READ_FAILED);
    }

    std::vector<std::string> imports;
    for (const auto& symbol : binary->getBinary()->symbols()) {
        auto flags{symbol.getFlags()};
        if (!flags) { continue; }
        if ((*flags & llvm::object::SymbolRef::SF_Undefined) == 0) { continue; }
        auto name{symbol.getName()};
        if (!name) { continue; }
        const auto source_name{codegen::strip_global_prefix(triple, name->str())};
        if (codegen::is_compiler_rt_symbol(triple, source_name)) {
            imports.emplace_back(source_name);
        }
    }
    std::ranges::sort(imports);
    const auto duplicates{std::ranges::unique(imports)};
    imports.erase(duplicates.begin(), duplicates.end());
    return imports;
}

auto find_self_calling_libcalls(std::string_view               asm_text,
                                const codegen::target_options& target_opts)
    -> std::vector<std::string> {
    const auto               triple{codegen::resolve_target_triple(target_opts.triple_str)};
    std::vector<std::string> found;
    std::string              current;

    for (auto line_range : std::views::split(asm_text, '\n')) {
        std::string_view line{line_range.begin(), line_range.end()};
        if (line.ends_with('\r')) { line.remove_suffix(1); }
        if (line.empty()) { continue; }

        // A label at column zero starts a function unless it's a local label
        if (!std::isspace(static_cast<u8>(line.front()))) {
            const auto colon{line.find(':')};
            if (colon == std::string_view::npos) { continue; }
            auto label{line.substr(0, colon)};
            if (label.size() >= 2 && label.front() == '"' && label.back() == '"') {
                label = label.substr(1, label.size() - 2);
            }
            // Column-zero comments such as `# %bb.0:` end in a colon too
            if (label.empty() || !std::ranges::all_of(label, is_identifier_char)) { continue; }
            if (!is_local_label(label)) {
                current = std::string{codegen::strip_global_prefix(triple, label)};
            }
            continue;
        }

        if (current.empty() || !codegen::is_runtime_libcall(triple, current)) { continue; }
        const auto first{line.find_first_not_of(" \t")};
        if (first == std::string_view::npos) { continue; }
        const auto body{line.substr(first)};
        // Directives and comments never call anything
        if (body.starts_with('.') || body.starts_with('#') || body.starts_with(';') ||
            body.starts_with("//")) {
            continue;
        }

        for (usize i{0}; i < body.size();) {
            if (!is_identifier_char(body[i])) {
                ++i;
                continue;
            }
            usize end{i};
            while (end < body.size() && is_identifier_char(body[end])) { ++end; }
            const auto token{codegen::strip_global_prefix(triple, body.substr(i, end - i))};
            if (token == current && !std::ranges::contains(found, current)) {
                found.emplace_back(current);
            }
            i = end;
        }
    }
    return found;
}

auto resolve_compiler_rt(const codegen::target_options&      target_opts,
                         const std::filesystem::path&        object,
                         const codegen::compiler_rt_options& options)
    -> stdx::result<stdx::option<std::filesystem::path>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    if (TRY(compiler_rt_imports(object, target_opts)).empty()) { return stdx::none; }
    const auto root{locate_compiler_rt(options)};
    if (!root) { return stdx::none; }

    const auto triple{codegen::resolve_target_triple(target_opts.triple_str)};
    const auto key{memo_key(target_opts, triple, *root)};
    auto&      state{memo()};

    const std::scoped_lock lock{state.mutex};
    if (const auto it{state.entries.find(key)}; it != state.entries.end()) {
        if (it->second.error) { return stdx::err{*it->second.error}; }
        return it->second.archive;
    }

    if (!state.dir) { state.dir = stdx::make_nullable_box<tempdir>("compiler_rt"); }
    const auto archive{state.dir->path / fmt::format("compiler_rt_{}.a", state.entries.size())};

    ++state.builds;
    archive_entry entry;
    if (auto built{build_archive(*root, target_opts, triple, archive)}; built) {
        entry.archive = archive;
    } else {
        entry.error = std::move(built.error());
    }
    const auto& stored{state.entries.emplace(key, std::move(entry)).first->second};
    if (stored.error) { return stdx::err{*stored.error}; }
    return stored.archive;
}

auto compiler_rt_builds() noexcept -> usize {
    auto&                  state{memo()};
    const std::scoped_lock lock{state.mutex};
    return state.builds;
}

} // namespace ghoti::runtime
