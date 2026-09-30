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
#include <lld/Common/CommonLinkerContext.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/COFF.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/MachO.h>
#include <llvm/Object/ObjectFile.h>
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

[[nodiscard]] auto emit_preamble(helpers::sema_test_context& test_ctx,
                                 bool                        for_test_executable = false)
    -> stdx::result<gir::module, codegen::diagnostic> {
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned", codegen::error::MODULE_LOAD_ERROR);
    }

    auto gir_mod{test_ctx.analyzer.emit_gir(test_ctx.root_mod, for_test_executable)};
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned during GIR emission",
                                         codegen::error::MODULE_LOAD_ERROR);
    }
    return gir_mod;
}

} // namespace

auto emit_llvm_ir(helpers::sema_test_context&       test_ctx,
                  llvm::LLVMContext&                context,
                  const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_llvm_ir(gir_mod, context, options);
}

auto emit_llvm_ir_executable(helpers::sema_test_context&       test_ctx,
                             llvm::LLVMContext&                context,
                             const codegen::optimizer_options& options,
                             std::string_view                  user_main_name)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_llvm_ir_executable(gir_mod, context, options, user_main_name);
}

auto emit_object(helpers::sema_test_context&       test_ctx,
                 llvm::LLVMContext&                context,
                 const std::filesystem::path&      output_path,
                 const codegen::target_options&    target_opts,
                 const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_object(gir_mod, context, target_opts, opt_options, output_path);
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
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_executable(
        gir_mod, context, target_opts, opt_options, output_path, linker_opts);
}

auto emit_test_executable(helpers::sema_test_context&          test_ctx,
                          llvm::LLVMContext&                   context,
                          const std::filesystem::path&         output_path,
                          const codegen::target_options&       target_opts,
                          const codegen::optimizer_options&    opt_options,
                          const codegen::extra_linker_options& linker_opts)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx, true))};
    return test_ctx.analyzer.emit_test_executable(
        gir_mod, context, target_opts, opt_options, output_path, linker_opts);
}

auto emit_static_lib(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts,
                     const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_static_library(
        gir_mod, context, target_opts, opt_options, output_path);
}

auto emit_dynamic_lib(helpers::sema_test_context&       test_ctx,
                      llvm::LLVMContext&                context,
                      const std::filesystem::path&      output_path,
                      const codegen::target_options&    target_opts,
                      const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_dynamic_library(
        gir_mod, context, target_opts, opt_options, output_path, default_linker_options());
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
    auto  ctx_idx{type_check_and_verify(source, imports)};
    auto& test_ctx{*ctx_idx.first};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const auto exe_stem{tempfile::make_temp_path("compile_and_run")};
    tempfile   exe_file{std::in_place, fmt::format("{}{}", exe_stem.string(), extension)};

    const auto emitted{emit_executable(test_ctx, context, exe_file.path, {}, {}, linker_opts)};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    const mock_argv args{exe_file.path.string()};
    return portable_exit_code(UNWRAP(spawn_child(args)));
}

auto missing_builtins(std::string_view source, const codegen::target_options& target_opts)
    -> std::vector<std::string> {
    auto  ctx_idx{type_check_and_verify(source, {})};
    auto& test_ctx{*ctx_idx.first};

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
    auto  ctx_idx{type_check_and_verify(source, imports)};
    auto& test_ctx{*ctx_idx.first};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const auto exe_stem{tempfile::make_temp_path("compile_and_run_captured")};
    tempfile   exe_file{std::in_place, fmt::format("{}{}", exe_stem.string(), extension)};

    const auto emitted{emit_executable(test_ctx, context, exe_file.path)};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    piped_process proc{mock_argv{exe_file.path.string()}};
    return run_output{
        .exit_code = portable_exit_code(UNWRAP(proc.close_stdin_and_wait())),
        .out       = string_utils::read_stream(proc.stdout_stream()),
        .err       = string_utils::read_stream(proc.stderr_stream()),
    };
}

auto compile_and_run_tests(std::string_view                source,
                           const std::vector<mock_file>&   imports,
                           const std::vector<std::string>& extra_args) -> u32 {
    auto  ctx_idx{type_check_and_verify(source, imports)};
    auto& test_ctx{*ctx_idx.first};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const auto exe_stem{tempfile::make_temp_path("compile_and_run_tests")};
    tempfile   exe_file{std::in_place, fmt::format("{}{}", exe_stem.string(), extension)};

    const auto emitted{emit_test_executable(test_ctx, context, exe_file.path, {}, {})};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    std::vector<std::string> child_argv{exe_file.path.string()};
    child_argv.insert(child_argv.end(), extra_args.begin(), extra_args.end());
    return portable_exit_code(UNWRAP(spawn_child(mock_argv{std::move(child_argv)})));
}

} // namespace ghoti::tests::helpers
