#include "helpers/codegen.hh"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <lld/Common/CommonLinkerContext.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/harness/hooks.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/runtime_libcalls.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/runtime/compiler_rt.hh"
#include "compiler/sema/analyzer.hh"
#include "ghoti/config.h"
#include "helpers/sema.hh"
#include "helpers/uir_diff.hh"
#include "support/string_utils.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

extern "C" {
auto harness_pre_main(i32, char**) -> void { ghoti::codegen::llvm_init_warmup(); }
auto harness_post_main(i32) -> void { ghoti::codegen::llvm_shutdown(); }
}

namespace ghoti::tests::helpers {

auto ir_text(llvm::Module& mod) -> std::string { return codegen::llvm_lowering::to_ir_string(mod); }

namespace {

// The context's program, analyzed by UIR Sema as the driver builds it
[[nodiscard]] auto analyze_program(helpers::sema_test_context& test_ctx,
                                   bool                        entry,
                                   bool                        for_tests,
                                   stdx::option<std::string>   triple = stdx::none)
    -> stdx::result<gsl::not_null<uir_sema_run*>, codegen::diagnostic> {
    const auto& ctx{test_ctx.analyzer.get_ctx()};
    auto        target_opts{ctx.target_opts};
    if (triple) { target_opts.triple_str = std::move(triple); }

    test_ctx.program.emplace(run_uir_sema_files(*test_ctx.loader,
                                                test_ctx.root_path,
                                                {.target_opts       = target_opts,
                                                 .runtime_safety    = ctx.runtime_safety,
                                                 .testing           = for_tests,
                                                 .build_mode        = ctx.build_mode,
                                                 .entry             = entry,
                                                 .deprecated_policy = ctx.deprecated_policy},
                                                test_ctx.libraries));
    auto& run{**test_ctx.program};
    if (run.out->stopped_at) {
        return codegen::make_codegen_err(
            fmt::format("Sema stopped at {}", run.out->stopped_at->what),
            codegen::error::MODULE_LOAD_ERROR);
    }
    if (!run.out->succeeded()) {
        return codegen::make_codegen_err(fmt::format("{}", run.out->diagnostics),
                                         codegen::error::MODULE_LOAD_ERROR);
    }

    return &run;
}

// A temporary path for an executable a helper builds
[[nodiscard]] auto temp_exe_path(std::string_view stem) -> std::filesystem::path {
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    return fmt::format("{}{}", tempfile::make_temp_path(stem).string(), extension);
}

// `source`'s program with `imports`, analyzed and built into `exe`
[[nodiscard]] auto build_program(std::string_view                     source,
                                 const std::vector<mock_file>&        imports,
                                 bool                                 for_tests,
                                 const codegen::extra_linker_options& linker_opts,
                                 const std::filesystem::path& exe) -> stdx::box<sema_test_context> {
    auto test_ctx{stdx::make_box<sema_test_context>(imports, TEST_FILENAME, source)};
    check_errors<syntax::diagnostics>(test_ctx->root_mod);

    llvm::LLVMContext context;
    const auto emitted{for_tests
                           ? emit_test_executable(*test_ctx, context, exe, {}, {}, linker_opts)
                           : emit_executable(*test_ctx, context, exe, {}, {}, linker_opts)};
    if (!emitted) { FAIL(fmt::format("{}", emitted.error())); }
    return test_ctx;
}

} // namespace

auto emit_llvm_ir(helpers::sema_test_context&       test_ctx,
                  llvm::LLVMContext&                context,
                  const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, false, false))};
    return run.analyzer->emit_llvm_ir(*run.out->gir, context, options);
}

auto emit_llvm_ir_executable(helpers::sema_test_context&       test_ctx,
                             llvm::LLVMContext&                context,
                             const codegen::optimizer_options& options,
                             std::string_view                  user_main_name)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    // The entry code is analyzed for the target it runs on
    const auto triple{
        options.target_machine
            ? stdx::option<std::string>{options.target_machine->getTargetTriple().str()}
            : stdx::none};
    auto& run{*TRY(analyze_program(test_ctx, true, false, triple))};
    return run.analyzer->emit_llvm_ir_executable(*run.out->gir, context, options, user_main_name);
}

auto emit_object(helpers::sema_test_context&       test_ctx,
                 llvm::LLVMContext&                context,
                 const std::filesystem::path&      output_path,
                 const codegen::target_options&    target_opts,
                 const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, false, false, target_opts.triple_str))};
    return run.analyzer->emit_object(*run.out->gir, context, target_opts, opt_options, output_path);
}

auto default_linker_options() -> codegen::extra_linker_options {
    codegen::extra_linker_options linker_opts;
    linker_opts.compiler_rt.emplace();
    return linker_opts;
}

auto emit_executable(helpers::sema_test_context&          test_ctx,
                     llvm::LLVMContext&                   context,
                     const std::filesystem::path&         output_path,
                     const codegen::target_options&       target_opts,
                     const codegen::optimizer_options&    opt_options,
                     const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, true, false, target_opts.triple_str))};
    return run.analyzer->emit_executable(
        *run.out->gir, context, target_opts, opt_options, output_path, linker_opts);
}

auto emit_test_executable(helpers::sema_test_context&          test_ctx,
                          llvm::LLVMContext&                   context,
                          const std::filesystem::path&         output_path,
                          const codegen::target_options&       target_opts,
                          const codegen::optimizer_options&    opt_options,
                          const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, true, true, target_opts.triple_str))};
    return run.analyzer->emit_test_executable(
        *run.out->gir, context, target_opts, opt_options, output_path, linker_opts);
}

auto emit_static_lib(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts,
                     const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, false, false, target_opts.triple_str))};
    return run.analyzer->emit_static_library(
        *run.out->gir, context, target_opts, opt_options, output_path);
}

auto emit_dynamic_lib(helpers::sema_test_context&       test_ctx,
                      llvm::LLVMContext&                context,
                      const std::filesystem::path&      output_path,
                      const codegen::target_options&    target_opts,
                      const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto& run{*TRY(analyze_program(test_ctx, false, false, target_opts.triple_str))};
    return run.analyzer->emit_dynamic_library(
        *run.out->gir, context, target_opts, opt_options, output_path, default_linker_options());
}

auto exported_symbols(const std::filesystem::path&   library,
                      const codegen::target_options& target_opts) -> std::vector<std::string> {
    const auto triple{codegen::resolve_target_triple(target_opts.triple_str)};
    auto       binary{llvm::object::createBinary(library.string())};
    REQUIRE(static_cast<bool>(binary));
    const auto* object{llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary())};
    REQUIRE(object != nullptr);

    std::vector<std::string> names;
    const auto               add{[&](llvm::StringRef name) {
        names.emplace_back(codegen::strip_global_prefix(triple, name));
    }};
    if (const auto* coff{llvm::dyn_cast<llvm::object::COFFObjectFile>(object)}) {
        for (const auto& entry : coff->export_directories()) {
            llvm::StringRef name;
            if (!entry.getSymbolName(name)) { add(name); }
        }
    } else if (const auto* macho{llvm::dyn_cast<llvm::object::MachOObjectFile>(object)}) {
        llvm::Error error{llvm::Error::success()};
        for (const auto& entry : macho->exports(error)) { add(entry.name()); }
        REQUIRE_FALSE(static_cast<bool>(error));
    } else if (const auto* elf{llvm::dyn_cast<llvm::object::ELFObjectFileBase>(object)}) {
        for (const auto& symbol : elf->getDynamicSymbolIterators()) {
            auto flags{symbol.getFlags()};
            auto name{symbol.getName()};
            if (!flags || !name) { continue; }
            if ((*flags & llvm::object::SymbolRef::SF_Undefined) == 0) { add(*name); }
        }
    } else {
        FAIL("unsupported shared library format");
    }
    return names;
}

auto portable_exit_code(u32 code) -> u32 {
#if GHOTI_WINDOWS
    const bool exception_status{(code & 0xF0000000U) == 0xC0000000U || code == 0x80000003U};
    if (code > 0xFFU && !exception_status) {
        FAIL(fmt::format("exit code {} isn't portable: POSIX only sees its low 8 bits ({})",
                         code,
                         code & 0xFFU));
    }
#endif
    return code;
}

auto compile_and_run(std::string_view                     source,
                     const std::vector<mock_file>&        imports,
                     const codegen::extra_linker_options& linker_opts) -> u32 {
    const tempfile exe{std::in_place, temp_exe_path("compile_and_run")};
    const auto     built{build_program(source, imports, false, linker_opts, exe.path)};
    return portable_exit_code(UNWRAP(spawn_child(mock_argv{exe.path.string()})));
}

auto uir_compile_and_run(std::string_view source, const std::vector<mock_file>& imports) -> u32 {
    return compile_and_run(source, imports);
}

auto missing_builtins(std::string_view source, const codegen::target_options& target_opts)
    -> std::vector<std::string> {
    sema_test_context test_ctx{{}, TEST_FILENAME, source};
    check_errors<syntax::diagnostics>(test_ctx.root_mod);

    llvm::LLVMContext context;
    const tempfile    object{"missing_builtins.o"};
    REQUIRE(emit_object(test_ctx, context, object.path, target_opts));
    auto imports{UNWRAP(runtime::compiler_rt_imports(object.path, target_opts))};
    if (imports.empty()) { return imports; }

    const auto archive{UNWRAP(runtime::resolve_compiler_rt(target_opts, object.path, {}))};
    if (!archive) { return imports; }
    const auto defined{archive_definitions(*archive, target_opts)};
    std::erase_if(imports, [&](const auto& name) { return std::ranges::contains(defined, name); });
    return imports;
}

auto archive_definitions(const std::filesystem::path&   archive,
                         const codegen::target_options& target_opts) -> std::vector<std::string> {
    const auto triple{codegen::resolve_target_triple(target_opts.triple_str)};
    auto       buffer{llvm::MemoryBuffer::getFile(archive.string())};
    REQUIRE(buffer);
    auto parsed{llvm::object::Archive::create((*buffer)->getMemBufferRef())};
    REQUIRE(static_cast<bool>(parsed));

    std::vector<std::string> defined;
    for (const auto& symbol : (*parsed)->symbols()) {
        defined.emplace_back(codegen::strip_global_prefix(triple, symbol.getName()));
    }
    return defined;
}

auto compile_and_run_captured(std::string_view source, const std::vector<mock_file>& imports)
    -> run_output {
    const tempfile exe{std::in_place, temp_exe_path("compile_and_run_captured")};
    const auto     built{build_program(source, imports, false, default_linker_options(), exe.path)};

    piped_process proc{mock_argv{exe.path.string()}};
    return run_output{
        .exit_code = portable_exit_code(UNWRAP(proc.close_stdin_and_wait())),
        .out       = string_utils::read_stream(proc.stdout_stream()),
        .err       = string_utils::read_stream(proc.stderr_stream()),
    };
}

auto compile_and_run_tests(std::string_view                source,
                           const std::vector<mock_file>&   imports,
                           const std::vector<std::string>& extra_args) -> u32 {
    const tempfile exe{std::in_place, temp_exe_path("compile_and_run_tests")};
    const auto     built{build_program(source, imports, true, default_linker_options(), exe.path)};

    std::vector<std::string> child_argv{exe.path.string()};
    child_argv.insert(child_argv.end(), extra_args.begin(), extra_args.end());
    return portable_exit_code(UNWRAP(spawn_child(mock_argv{std::move(child_argv)})));
}

} // namespace ghoti::tests::helpers
