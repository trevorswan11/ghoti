#include "compiler/sema/analyzer.hh"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/util>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/ast/statement.hh"
#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/codegen/llvm_optimizer.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/soft_f80.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/runtime/compiler_rt.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/symbol_collector.hh"
#include "compiler/sema/passes/type_checker.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/error.hh"
#include "support/tempfile.hh"

namespace ghoti::sema {

namespace {

[[nodiscard]] auto make_tmp_obj(const std::filesystem::path& path) -> tempfile {
    auto temp_obj_path{path};
    temp_obj_path.replace_extension(".tmp.o");
    return tempfile{std::in_place, temp_obj_path};
}

[[nodiscard]] auto is_argv_slice_type(const type* t) noexcept -> bool {
    if (!t || t->get_kind() != type_kind::SLICE) { return false; }
    const auto outer{t->get_data().as_opt<types::slice>()};
    if (!outer || outer->null_terminated || outer->underlying.get_kind() != type_kind::SLICE) {
        return false;
    }
    const auto inner{outer->underlying.get_data().as_opt<types::slice>()};
    if (!inner || !inner->null_terminated) { return false; }
    const auto elem{as_integer(inner->underlying)};
    return elem && elem->bits == 8 && !elem->is_signed;
}

[[nodiscard]] auto export_roots(const gir::module& mod) -> std::vector<std::string_view> {
    std::vector<std::string_view> roots;
    for (const auto* fn : mod.get_functions()) {
        if (fn->get_linkage() == gir::linkage::EXPORT) { roots.emplace_back(fn->get_name()); }
    }
    for (const auto* g : mod.get_globals()) {
        if (g->linkage == gir::linkage::EXPORT) { roots.emplace_back(g->name); }
    }
    return roots;
}

// A library's interface is what it exports; everything else it holds (its copy of `std`, say)
// stays internal so it can't collide with the program's own. A `panic_handler` / `assert_handler`
// override stays visible but weak, since the program linking it defines its own that must win,
// unless `keep_handlers` is off
auto internalize_library(gir::module& mod, bool keep_handlers) -> void {
    constexpr std::array handlers{std::string_view{"panic_handler"},
                                  std::string_view{"assert_handler"},
                                  std::string_view{"expect_handler"},
                                  std::string_view{"require_handler"},
                                  std::string_view{"skip_handler"}};
    for (auto* fn : mod.get_functions()) {
        if (fn->get_linkage() == gir::linkage::INTERNAL) { continue; }
        const auto symbol{fn->get_link_name().empty() ? fn->get_name() : fn->get_link_name()};
        if (keep_handlers && std::ranges::contains(handlers, symbol)) {
            fn->set_weak(true);
        } else if (fn->get_linkage() == gir::linkage::PUBLIC) {
            fn->set_linkage(gir::linkage::INTERNAL);
        }
    }
    for (auto* g : mod.get_globals()) {
        if (g->linkage == gir::linkage::PUBLIC) { g->linkage = gir::linkage::INTERNAL; }
    }
}

constexpr std::array supported_archs{
    "x86_64",
    "aarch64",
    "riscv32",
    "riscv64",
    "arm",
    "thumb",
    "loongarch64",
};

auto prune_to_test_reachable(gir::module& gir_module) -> void {
    std::vector<std::string_view> roots;
    for (const auto* fn : gir_module.get_test_functions()) { roots.emplace_back(fn->get_name()); }
    // `std/start.gh`'s exports run the tests
    for (const auto root : export_roots(gir_module)) { roots.emplace_back(root); }
    roots.emplace_back("test_runner");
    roots.emplace_back("expect_handler");
    roots.emplace_back("require_handler");
    roots.emplace_back("skip_handler");
    gir_module.prune_unreachable(roots);
}

// The LLVM-level names of every `export` function and global, which must survive internalizing
[[nodiscard]] auto exported_symbol_names(const gir::module& mod) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto* fn : mod.get_functions()) {
        if (fn->get_linkage() != gir::linkage::EXPORT) { continue; }
        names.emplace_back(fn->get_link_name().empty() ? fn->get_name() : fn->get_link_name());
    }
    for (const auto* g : mod.get_globals()) {
        if (g->linkage == gir::linkage::EXPORT) {
            names.emplace_back(g->link_name.empty() ? g->name : g->link_name);
        }
    }
    return names;
}

[[nodiscard]] auto with_target_machine(const codegen::optimizer_options& opt_options,
                                       const codegen::target_options&    target_opts,
                                       llvm::TargetMachine&              target_machine)
    -> codegen::optimizer_options {
    codegen::optimizer_options opts{opt_options};
    opts.target_machine = &target_machine;
    if (opts.level == codegen::opt_level::O0 && target_opts.level != codegen::opt_level::O0) {
        opts.level = target_opts.level;
    }
    return opts;
}

// Libraries named by `extern("lib", ...)` declarations join the ones given on the command line
[[nodiscard]] auto merged_libraries(const codegen::extra_linker_options& linker_opts,
                                    const gir::module& gir_module) -> std::vector<std::string> {
    std::vector<std::string> merged{linker_opts.libraries.begin(), linker_opts.libraries.end()};
    for (auto& lib : gir_module.get_required_libraries()) { merged.emplace_back(std::move(lib)); }
    return merged;
}

// What each `extern("lib", ...)` library supplies, for a Windows import library the link writes
[[nodiscard]] auto library_imports(const gir::module& gir_module)
    -> std::vector<codegen::library_import> {
    std::vector<codegen::library_import> imports;
    for (auto& [library, symbol, data] : gir_module.get_library_imports()) {
        imports.emplace_back<codegen::library_import>(
            {std::move(library), std::move(symbol), data});
    }
    return imports;
}

// A DLL exports only what is marked `dllexport`, so mark what an ELF or Mach-O shared library
// would export: every defined, externally linked symbol that isn't `@[visibility(.hidden)]`
auto mark_dll_exports(llvm::Module& llvm_mod) -> void {
    const auto mark{[](llvm::GlobalValue& value) {
        if (value.isDeclaration() || value.hasLocalLinkage() || !value.hasDefaultVisibility()) {
            return;
        }
        // LLVM's own bookkeeping arrays (`llvm.used`, ...) aren't symbols
        if (value.hasAppendingLinkage() || value.getName().starts_with("llvm.")) { return; }
        value.setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    }};
    for (auto& fn : llvm_mod) { mark(fn); }
    for (auto& global : llvm_mod.globals()) { mark(global); }
}

// Builds the compiler builtins archive when the object calls into it and none was given
[[nodiscard]] auto add_compiler_rt(codegen::extra_linker_options& linker_opts,
                                   const codegen::target_options& target_opts,
                                   const std::filesystem::path&   object)
    -> stdx::result<void, codegen::diagnostic> {
    if (linker_opts.builtins || !linker_opts.compiler_rt) { return {}; }
    linker_opts.builtins =
        TRY(runtime::resolve_compiler_rt(target_opts, object, *linker_opts.compiler_rt));
    return {};
}

[[nodiscard]] auto check_freestanding_entry(const llvm::Triple& triple, std::string_view artifact)
    -> stdx::result<void, codegen::diagnostic> {
    if (codegen::can_emit_freestanding_entry(triple)) { return {}; }
    return codegen::make_codegen_err(
        fmt::format("cannot build {} for target '{}': ghoti links no C runtime on Linux and has no "
                    "freestanding entry point for the '{}' architecture (supported: {})",
                    artifact,
                    triple.str(),
                    codegen::normalized_target_arch(triple),
                    fmt::join(supported_archs, ", ")),
        codegen::error::UNSUPPORTED_TARGET);
}

// `@[inline(.always)]` is honored even at -O0, where only the always-inliner runs
[[nodiscard]] auto has_always_inline(const llvm::Module& llvm_mod) -> bool {
    return std::ranges::any_of(llvm_mod, [](const llvm::Function& fn) {
        if (fn.hasFnAttribute(llvm::Attribute::AlwaysInline)) { return true; }
        // `@call(.always_inline, ...)` marks the call site, not the function
        for (const auto& block : fn) {
            for (const auto& inst : block) {
                const auto* call{llvm::dyn_cast<llvm::CallBase>(&inst)};
                if (call && call->hasFnAttr(llvm::Attribute::AlwaysInline)) { return true; }
            }
        }
        return false;
    });
}

// Whether an import, at any depth, reaches a module that failed. One reached only through an import
// cycle can finish resolving before a module it depends on is poisoned and keep a clean state of
// its own, so lowering it would walk untyped code.
[[nodiscard]] auto reaches_failed_module(mod::module& root) -> bool {
    ankerl::unordered_dense::set<const mod::module*> seen{&root};
    std::vector<mod::module*>                        frontier{&root};
    while (!frontier.empty()) {
        auto* current{frontier.back()};
        frontier.pop_back();
        for (const auto import_id : current->import_nodes) {
            const auto import_type{current->get_sema_type_opt(import_id)};
            if (!import_type) { continue; }
            const auto data{import_type->get_data().as_opt<types::module>()};
            if (!data) { continue; }
            auto& imported{data->imported};
            if (imported.is_poisoned() || imported.is_errored()) { return true; }
            if (seen.insert(&imported).second) { frontier.emplace_back(&imported); }
        }
    }
    return false;
}

// LLVM's `target-features` replaces the target's whole feature string for a function, so an
// `@[target_features(...)]` function gets the target's features with its own added on
auto apply_target_features(llvm::Module& llvm_mod, const llvm::TargetMachine* target_machine)
    -> void {
    const auto base{target_machine ? target_machine->getTargetFeatureString().str()
                                   : std::string{}};
    for (auto& fn : llvm_mod) {
        if (!fn.hasFnAttribute("ghoti-target-features")) { continue; }
        const auto extra{fn.getFnAttribute("ghoti-target-features").getValueAsString().str()};
        fn.removeFnAttr("ghoti-target-features");
        fn.addFnAttr("target-features", base.empty() ? extra : fmt::format("{},{}", base, extra));
        if (target_machine) { fn.addFnAttr("target-cpu", target_machine->getTargetCPU()); }
    }
}

[[nodiscard]] auto verify_and_optimize(llvm::Module&                     llvm_mod,
                                       const codegen::optimizer_options& options)
    -> stdx::result<void, codegen::diagnostic> {
    apply_target_features(llvm_mod, options.target_machine);
    TRY(codegen::soften_f80(llvm_mod));
    std::string              err_str;
    llvm::raw_string_ostream os{err_str};
    if (llvm::verifyModule(llvm_mod, &os)) {
        return codegen::make_codegen_err(err_str, codegen::error::VERIFICATION_FAILED);
    }

    if (options.level != codegen::opt_level::O0 || options.debug_logging || options.time_passes ||
        has_always_inline(llvm_mod)) {
        codegen::llvm_optimizer optimizer{llvm_mod.getContext()};
        TRY(optimizer.optimize(llvm_mod, options));
    }
    return {};
}

} // namespace

auto analyzer::analyze(const std::filesystem::path& entry_path, bool for_test_executable)
    -> stdx::result<gir::module, diagnostic> {
    PROFILE_FUNCTION();
    if (for_test_executable) { ctx_.testing = true; }
    // An unknown `--cpu` / `--features` would otherwise surface at the first `cpu.has(...)`
    if (auto cpu{codegen::resolve_target_cpu(ctx_.target_opts)}; !cpu) {
        return make_sema_err(cpu.error().get_message().value_or("Unknown target CPU"),
                             error::UNSUPPORTED_TARGET);
    }
    auto module_result{modules_.try_get_file_module(entry_path)};
    if (!module_result) {
        return make_sema_err(std::move(module_result.error()).get_message(),
                             error::MODULE_LOAD_ERROR);
    }

    auto module{*module_result};
    if (module->diagnostics.is<syntax::diagnostics>()) { module->print_diagnostics(error_stream_); }

    // A lenient analyzer still runs sema over whatever parsed
    if (module->is_errored() && tolerate_syntax_errors_) {
        module->state = mod::module_state::PARSED;
    }

    // An errored module's AST is incomplete/inconsistent; it can't proceed past this point.
    if (module->is_errored()) { return gir::module{*module, ctx_.arena}; }

    collect_symbols(*module);
    resolve_types(*module);
    modules_.print_all_warnings(error_stream_);

    if (module->is_poisoned() || reaches_failed_module(*module)) {
        modules_.print_all_diagnostics(error_stream_);
        return gir::module{*module, ctx_.arena};
    }

    if (entry_point_ || for_test_executable) {
        TRY(attach_start_module(*module, for_test_executable));
        if (module->is_poisoned()) {
            modules_.print_all_diagnostics(error_stream_);
            return gir::module{*module, ctx_.arena};
        }
    }

    auto gir_mod{emit_gir(*module, for_test_executable)};
    if (module->is_poisoned()) {
        modules_.print_all_diagnostics(error_stream_);
        return gir::module{*module, ctx_.arena};
    }

    check_types(gir_mod, *module);
    if (module->is_poisoned()) {
        modules_.print_all_diagnostics(error_stream_);
        return gir::module{*module, ctx_.arena};
    }

    return gir_mod;
}

auto analyzer::collect_symbols(mod::module& module) -> mod::module_state {
    return symbol_collector::collect_symbols(module, ctx_);
}

auto analyzer::resolve_types(mod::module& module) -> mod::module_state {
    return type_resolver::resolve_types(module, ctx_);
}

auto analyzer::attach_start_module(mod::module& root, bool for_test_executable)
    -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    if (ctx_.start_module) { return {}; }
    // An invalid `main` or `test_runner` is reported by its own check, with no entry code to call
    // it
    const auto valid{for_test_executable ? validate_test_entry(root) : validate_main_entry(root)};
    if (!valid) { return {}; }

    const auto path{mod::find_start()};
    if (!path) {
        return make_sema_err("cannot find the program entry code `lib/std/start.gh`; set "
                             "GHOTI_START to its path",
                             error::MODULE_LOAD_ERROR);
    }
    std::ifstream file{*path, std::ios::binary};
    if (!file) {
        return make_sema_err(fmt::format("cannot read the program entry code '{}'", path->string()),
                             error::MODULE_LOAD_ERROR);
    }
    const std::string source{std::istreambuf_iterator<char>{file}, {}};

    auto& start{modules_.get_or_create_start_module(*path, source)};
    if (!start.is_errored()) {
        ctx_.entry_root   = &root;
        ctx_.start_module = &start;
        // The entry code's `@cfg(testing)` picks between `main` and the tests by what is built
        const auto testing{std::exchange(ctx_.testing, for_test_executable)};
        collect_symbols(start);
        resolve_types(start);
        ctx_.testing = testing;
        report_entry_errors_at(root, start, for_test_executable ? "test_runner" : "main");
        if (root.is_poisoned()) { return {}; }
    }
    if (start.is_errored() || start.is_poisoned()) {
        modules_.print_all_diagnostics(error_stream_);
        return make_sema_err("the program entry code `lib/std/start.gh` failed to compile",
                             error::MODULE_LOAD_ERROR);
    }
    return {};
}

auto analyzer::report_entry_errors_at(mod::module&     root,
                                      mod::module&     start,
                                      std::string_view entry_name) -> void {
    const auto start_diags{start.diagnostics.as_opt<diagnostics>()};
    if (!start.is_poisoned() || !start_diags) { return; }

    stdx::option<source_location> entry_location;
    if (root.root_table_idx) {
        if (const auto sym{registry_.get(*root.root_table_idx).get_opt(entry_name)}) {
            entry_location = sym->get_symbol_location(root);
        }
    }

    auto moved{start_diags->create_new()};
    auto kept{start_diags->create_new()};
    for (const auto& diag : *start_diags) {
        if (diag.get_error() != error::COMPILE_ERROR_REACHED || diag.origin()) {
            kept.push_back(diag);
            continue;
        }
        const auto at{diag.to_formattable().location};
        const auto required_by{at ? fmt::format("{}:{}", start.path.generic_string(), *at)
                                  : start.path.generic_string()};
        moved.emplace_back(fmt::format("{} (required by {})",
                                       diag.get_message().value_or("compile error reached"),
                                       required_by),
                           error::COMPILE_ERROR_REACHED,
                           entry_location);
    }
    if (moved.empty()) { return; }

    root.absorb_sema_diagnostics(std::move(moved));
    if (kept.empty()) {
        start.diagnostics = stdx::monostate{};
        start.state       = mod::module_state::TYPE_RESOLVED;
    } else {
        start.diagnostics.emplace<diagnostics>(std::move(kept));
    }
}

auto analyzer::emit_gir(mod::module& module, bool for_test_executable) -> gir::module {
    if ((entry_point_ || for_test_executable) && !ctx_.start_module) {
        if (auto attached{attach_start_module(module, for_test_executable)}; !attached) {
            error_stream_ << attached.error().get_message().value_or("") << '\n';
        }
        if (module.is_poisoned()) {
            modules_.print_all_diagnostics(error_stream_);
            return gir::module{module, ctx_.arena};
        }
    }
    gir::emitter emitter{ctx_, module};
    return emitter.emit(for_test_executable);
}

auto analyzer::check_types(gir::module& gir_module, mod::module& ast_module) -> mod::module_state {
    return type_checker::check_types(gir_module, ast_module, ctx_);
}

auto analyzer::emit_llvm_ir(gir::module&                      gir_module,
                            llvm::LLVMContext&                context,
                            const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    codegen::llvm_lowering lowering{context, gir_module.get_ast_module().path.string()};
    if (options.target_machine) {
        lowering.module().setDataLayout(options.target_machine->createDataLayout());
        lowering.module().setTargetTriple(options.target_machine->getTargetTriple());
    }
    auto llvm_mod{lowering.lower(gir_module)};
    TRY(verify_and_optimize(*llvm_mod, options));
    return llvm_mod;
}

auto analyzer::emit_llvm_ir_executable(gir::module&                      gir_module,
                                       llvm::LLVMContext&                context,
                                       const codegen::optimizer_options& options,
                                       std::string_view                  user_main_name)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    const auto effective_main_name{user_main_name == "main" && ctx_.user_main_name != "main"
                                       ? std::string_view{ctx_.user_main_name}
                                       : user_main_name};
    // `std/start.gh`'s exports call `main`
    auto roots{export_roots(gir_module)};
    roots.emplace_back(effective_main_name);
    gir_module.prune_unreachable(roots);
    codegen::llvm_lowering lowering{context, gir_module.get_ast_module().path.string()};
    if (options.target_machine) {
        const llvm::Triple triple{options.target_machine->getTargetTriple()};
        TRY(check_freestanding_entry(triple, "an executable"));
        lowering.module().setDataLayout(options.target_machine->createDataLayout());
        lowering.module().setTargetTriple(triple);
    }
    auto llvm_mod{lowering.lower_executable(gir_module, effective_main_name)};

    codegen::optimizer_options exe_opts{options};
    exe_opts.internalize = true;
    exe_opts.preserved_symbols.emplace_back("main");
    for (auto& name : exported_symbol_names(gir_module)) {
        exe_opts.preserved_symbols.emplace_back(std::move(name));
    }
    TRY(verify_and_optimize(*llvm_mod, exe_opts));
    return llvm_mod;
}

auto analyzer::emit_llvm_ir_test_executable(gir::module&                      gir_module,
                                            llvm::LLVMContext&                context,
                                            const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    PROFILE_FUNCTION();
    prune_to_test_reachable(gir_module);
    codegen::llvm_lowering lowering{context, gir_module.get_ast_module().path.string()};
    if (options.target_machine) {
        const llvm::Triple triple{options.target_machine->getTargetTriple()};
        TRY(check_freestanding_entry(triple, "a test executable"));
        lowering.module().setDataLayout(options.target_machine->createDataLayout());
        lowering.module().setTargetTriple(triple);
    }
    auto llvm_mod{lowering.lower_test_executable(gir_module, ctx_.user_main_name)};
    TRY(verify_and_optimize(*llvm_mod, options));
    return llvm_mod;
}

auto analyzer::lower_artifact(gir::module&                      gir_module,
                              llvm::LLVMContext&                context,
                              const codegen::optimizer_options& options,
                              build_artifact                    artifact)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    switch (artifact) {
    case build_artifact::EXECUTABLE: return emit_llvm_ir_executable(gir_module, context, options);
    case build_artifact::TEST_EXECUTABLE:
        return emit_llvm_ir_test_executable(gir_module, context, options);
    case build_artifact::LIBRARY:
        gir_module.prune_unreachable(export_roots(gir_module));
        internalize_library(gir_module, library_keeps_handlers_);
        break;
    case build_artifact::OBJECT: gir_module.prune_unreachable({}); break;
    }
    return emit_llvm_ir(gir_module, context, options);
}

auto analyzer::emit_llvm_ir_text(gir::module&                      gir_module,
                                 const codegen::target_options&    target_opts,
                                 const codegen::optimizer_options& opt_options,
                                 build_artifact                    artifact)
    -> stdx::result<std::string, codegen::diagnostic> {
    PROFILE_FUNCTION();
    llvm::LLVMContext context;
    auto              target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto        opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto              llvm_mod{TRY(lower_artifact(gir_module, context, opts, artifact))};
    return codegen::llvm_lowering::to_ir_string(*llvm_mod);
}

auto analyzer::emit_asm_text(gir::module&                      gir_module,
                             const codegen::target_options&    target_opts,
                             const codegen::optimizer_options& opt_options,
                             build_artifact                    artifact)
    -> stdx::result<std::string, codegen::diagnostic> {
    PROFILE_FUNCTION();
    llvm::LLVMContext context;
    auto              target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto        opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto              llvm_mod{TRY(lower_artifact(gir_module, context, opts, artifact))};
    return codegen::emit_asm_string(*llvm_mod, *target_machine);
}

auto analyzer::validate_main_entry(const mod::module& root_module) const
    -> stdx::result<void, diagnostic> {
    if (!root_module.root_table_idx) {
        return make_sema_err("missing root module", error::MODULE_LOAD_ERROR);
    }

    // `@export(f, .{ .name = "main" })` picks the entry point, and needn't be `pub`
    const auto exported_main{ctx_.exports.named_by_program("main")};
    if (exported_main && exported_main->owner && exported_main->owner != root_module) {
        return make_sema_err("Only the root module can export a function as 'main'",
                             error::TYPE_MISMATCH);
    }

    const std::string_view main_name{ctx_.user_main_name};
    const auto&            root_table{registry_.get(*root_module.root_table_idx)};
    const auto             main_sym_opt{root_table.get_opt(main_name)};
    if (!main_sym_opt) {
        return make_sema_err(fmt::format("missing required '{}' function", main_name),
                             error::TYPE_MISMATCH);
    }

    const auto& main_sym{*main_sym_opt};
    if (!exported_main && !main_sym.is_public(root_module)) {
        return make_sema_err("'main' function must be declared 'pub'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    if (main_sym.get_kind_opt() != symbol_kind::CALLABLE) {
        return make_sema_err("'main' must be a callable function",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto node_sym{main_sym.get_data().as_opt<symbols::node_t>()};
    if (!node_sym) {
        return make_sema_err("'main' must be a user-defined function",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }
    if (const auto decl{root_module.ast.get_as_opt<ast::decl_stmt>(*node_sym)};
        decl && decl->has_modifier(ast::decl_modifiers::EXTERN)) {
        return make_sema_err("'main' must be defined in this module, not declared 'extern'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto sema_type_opt{root_module.get_sema_type_opt(*node_sym)};
    if (!sema_type_opt) {
        return make_sema_err("failed to resolve type for 'main'",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    const auto& sema_type{*sema_type_opt};
    if (sema_type.get_kind() != type_kind::FUNCTION) {
        return make_sema_err("'main' must have function type",
                             error::TYPE_MISMATCH,
                             main_sym.get_symbol_location(root_module));
    }

    // Its parameters and return type are the entry code's to check
    return {};
}

auto analyzer::validate_test_entry(const mod::module& root_module) const
    -> stdx::result<void, diagnostic> {
    if (!root_module.root_table_idx) {
        return make_sema_err("missing root module", error::MODULE_LOAD_ERROR);
    }

    // No user-defined `test_runner` -> the `weak` `builtin` default is used,
    const auto& root_table{registry_.get(*root_module.root_table_idx)};
    const auto  runner_sym_opt{root_table.get_opt("test_runner")};
    if (!runner_sym_opt) { return {}; }

    const auto&                runner_sym{*runner_sym_opt};
    constexpr std::string_view want{
        "'test_runner' must have signature 'fn(args: [][:0]u8, tests: []Test): i32'"};
    const auto loc{runner_sym.get_symbol_location(root_module)};

    if (runner_sym.get_kind_opt() != symbol_kind::CALLABLE) {
        return make_sema_err(
            fmt::format("{} (it is not a function)", want), error::TYPE_MISMATCH, loc);
    }

    const auto node_sym{runner_sym.get_data().as_opt<symbols::node_t>()};
    if (!node_sym) {
        return make_sema_err(fmt::format("{} (it must be a user-defined function)", want),
                             error::TYPE_MISMATCH,
                             loc);
    }

    const auto sema_type_opt{root_module.get_sema_type_opt(*node_sym)};
    if (!sema_type_opt || sema_type_opt->get_kind() != type_kind::FUNCTION) {
        return make_sema_err(fmt::format("{}", want), error::TYPE_MISMATCH, loc);
    }

    const auto& fn_data{sema_type_opt->get_data().as<types::function>()};
    if (fn_data.params.size() != 2 || !is_argv_slice_type(fn_data.params[0]) ||
        fn_data.params[1] == nullptr || fn_data.params[1]->get_kind() != type_kind::SLICE) {
        return make_sema_err(fmt::format("{}", want), error::TYPE_MISMATCH, loc);
    }

    const auto tests_slice{fn_data.params[1]->get_data().as_opt<types::slice>()};
    if (!tests_slice || tests_slice->null_terminated ||
        tests_slice->underlying.get_kind() != type_kind::STRUCT) {
        return make_sema_err(fmt::format("{} (the second parameter must be '[]Test')", want),
                             error::TYPE_MISMATCH,
                             loc);
    }

    if (!is_i32(fn_data.return_type)) {
        return make_sema_err(
            fmt::format("{} (it must return 'i32')", want), error::TYPE_MISMATCH, loc);
    }

    return {};
}

auto analyzer::emit_object(gir::module&                      gir_module,
                           const codegen::target_options&    target_opts,
                           const codegen::optimizer_options& opt_options,
                           const std::filesystem::path&      output_path)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_object(gir_module, context, target_opts, opt_options, output_path);
}

auto analyzer::emit_object(gir::module&                      gir_module,
                           llvm::LLVMContext&                context,
                           const codegen::target_options&    target_opts,
                           const codegen::optimizer_options& opt_options,
                           const std::filesystem::path&      output_path)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    auto       target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto       llvm_mod{TRY(lower_artifact(gir_module, context, opts, build_artifact::OBJECT))};
    return codegen::emit_object_file(*llvm_mod, *target_machine, output_path);
}

auto analyzer::emit_test_executable(gir::module&                         gir_module,
                                    const codegen::target_options&       target_opts,
                                    const codegen::optimizer_options&    opt_options,
                                    const std::filesystem::path&         output_path,
                                    const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_test_executable(
        gir_module, context, target_opts, opt_options, output_path, linker_opts);
}

auto analyzer::emit_test_executable(gir::module&                         gir_module,
                                    llvm::LLVMContext&                   context,
                                    const codegen::target_options&       target_opts,
                                    const codegen::optimizer_options&    opt_options,
                                    const std::filesystem::path&         output_path,
                                    const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    return link_artifact(gir_module,
                         context,
                         target_opts,
                         opt_options,
                         output_path,
                         linker_opts,
                         build_artifact::TEST_EXECUTABLE);
}

auto analyzer::emit_executable(gir::module&                         gir_module,
                               const codegen::target_options&       target_opts,
                               const codegen::optimizer_options&    opt_options,
                               const std::filesystem::path&         output_path,
                               const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_executable(gir_module, context, target_opts, opt_options, output_path, linker_opts);
}

auto analyzer::emit_executable(gir::module&                         gir_module,
                               llvm::LLVMContext&                   context,
                               const codegen::target_options&       target_opts,
                               const codegen::optimizer_options&    opt_options,
                               const std::filesystem::path&         output_path,
                               const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    return link_artifact(gir_module,
                         context,
                         target_opts,
                         opt_options,
                         output_path,
                         linker_opts,
                         build_artifact::EXECUTABLE);
}

auto analyzer::emit_static_library(gir::module&                         gir_module,
                                   const codegen::target_options&       target_opts,
                                   const codegen::optimizer_options&    opt_options,
                                   const std::filesystem::path&         output_path,
                                   const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_static_library(
        gir_module, context, target_opts, opt_options, output_path, linker_opts);
}

auto analyzer::emit_static_library(gir::module&                         gir_module,
                                   llvm::LLVMContext&                   context,
                                   const codegen::target_options&       target_opts,
                                   const codegen::optimizer_options&    opt_options,
                                   const std::filesystem::path&         output_path,
                                   const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    auto       target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto       llvm_mod{TRY(lower_artifact(gir_module, context, opts, build_artifact::LIBRARY))};
    const auto temp_obj_path{make_tmp_obj(output_path)};
    TRY(codegen::emit_object_file(*llvm_mod, *target_machine, temp_obj_path));

    std::vector<std::filesystem::path> objects;
    objects.reserve(linker_opts.objects.size() + 1);
    objects.emplace_back(temp_obj_path);
    for (const auto& obj : linker_opts.objects) { objects.emplace_back(obj); }
    return codegen::create_static_library(output_path, objects, target_opts);
}

auto analyzer::emit_dynamic_library(gir::module&                         gir_module,
                                    const codegen::target_options&       target_opts,
                                    const codegen::optimizer_options&    opt_options,
                                    const std::filesystem::path&         output_path,
                                    const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    llvm::LLVMContext context;
    return emit_dynamic_library(
        gir_module, context, target_opts, opt_options, output_path, linker_opts);
}

auto analyzer::emit_dynamic_library(gir::module&                         gir_module,
                                    llvm::LLVMContext&                   context,
                                    const codegen::target_options&       target_opts,
                                    const codegen::optimizer_options&    opt_options,
                                    const std::filesystem::path&         output_path,
                                    const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    PROFILE_FUNCTION();
    if (target_opts.reloc != codegen::reloc_model::PIC_) {
        return codegen::make_codegen_err("Dynamic libraries require PIC relocation mode enabled",
                                         codegen::error::ILLEGAL_DYLIB_RELOC_MODE);
    }
    auto       target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto       llvm_mod{TRY(lower_artifact(gir_module, context, opts, build_artifact::LIBRARY))};
    if (target_machine->getTargetTriple().isOSBinFormatCOFF()) { mark_dll_exports(*llvm_mod); }
    const auto temp_obj_path{make_tmp_obj(output_path)};
    TRY(codegen::emit_object_file(*llvm_mod, *target_machine, temp_obj_path));
    auto       libraries{merged_libraries(linker_opts, gir_module)};
    const auto imports{library_imports(gir_module)};
    auto       effective_linker_opts{linker_opts};
    effective_linker_opts.libraries = libraries;
    effective_linker_opts.imports   = imports;
    TRY(add_compiler_rt(effective_linker_opts, target_opts, temp_obj_path));
    return codegen::link_dynamic_library(
        temp_obj_path, output_path, target_opts, effective_linker_opts);
}

auto analyzer::link_artifact(gir::module&                         gir_module,
                             llvm::LLVMContext&                   context,
                             const codegen::target_options&       target_opts,
                             const codegen::optimizer_options&    opt_options,
                             const std::filesystem::path&         output_path,
                             const codegen::extra_linker_options& linker_opts,
                             build_artifact artifact) -> stdx::result<void, codegen::diagnostic> {
    auto       target_machine{TRY(codegen::create_target_machine(target_opts))};
    const auto opts{with_target_machine(opt_options, target_opts, *target_machine)};
    auto       llvm_mod{TRY(lower_artifact(gir_module, context, opts, artifact))};
    const auto temp_obj_path{make_tmp_obj(output_path)};
    TRY(codegen::emit_object_file(*llvm_mod, *target_machine, temp_obj_path));

    auto       libraries{merged_libraries(linker_opts, gir_module)};
    const auto imports{library_imports(gir_module)};
    auto       effective_linker_opts{linker_opts};
    effective_linker_opts.libraries = libraries;
    effective_linker_opts.imports   = imports;
    TRY(add_compiler_rt(effective_linker_opts, target_opts, temp_obj_path));
    return codegen::link_executable(temp_obj_path, output_path, target_opts, effective_linker_opts);
}

} // namespace ghoti::sema
