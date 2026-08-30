#include "helpers/codegen.hh"

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
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/harness/hooks.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/module.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

extern "C" {
auto harness_pre_main(i32, char**) -> void { ghoti::codegen::llvm_init_warmup(); }
auto harness_post_main(i32) -> void { ghoti::codegen::llvm_shutdown(); }
}

namespace ghoti::tests::helpers {

auto ir_text(llvm::Module& mod) -> std::string {
    std::string              out;
    llvm::raw_string_ostream os{out};
    mod.print(os, nullptr);
    return out;
}

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

auto emit_executable(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts,
                     const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx))};
    return test_ctx.analyzer.emit_executable(
        gir_mod, context, target_opts, opt_options, output_path);
}

auto emit_test_executable(helpers::sema_test_context&       test_ctx,
                          llvm::LLVMContext&                context,
                          const std::filesystem::path&      output_path,
                          const codegen::target_options&    target_opts,
                          const codegen::optimizer_options& opt_options)
    -> stdx::result<void, codegen::diagnostic> {
    auto gir_mod{TRY(emit_preamble(test_ctx, true))};
    return test_ctx.analyzer.emit_test_executable(
        gir_mod, context, target_opts, opt_options, output_path, {});
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

auto compile_and_run(std::string_view source, const std::vector<mock_file>& imports) -> u32 {
    auto  ctx_idx{type_check_and_verify(source, imports)};
    auto& test_ctx{*ctx_idx.first};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const auto exe_stem{tempfile::make_temp_path("compile_and_run")};
    tempfile   exe_file{std::in_place, fmt::format("{}{}", exe_stem.string(), extension)};

    const auto emitted{emit_executable(test_ctx, context, exe_file.path)};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    const mock_argv args{exe_file.path.string()};
    return UNWRAP(spawn_child(args));
}

auto compile_and_run_tests(std::string_view source, const std::vector<mock_file>& imports) -> u32 {
    auto  ctx_idx{type_check_and_verify(source, imports)};
    auto& test_ctx{*ctx_idx.first};

    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const auto exe_stem{tempfile::make_temp_path("compile_and_run_tests")};
    tempfile   exe_file{std::in_place, fmt::format("{}{}", exe_stem.string(), extension)};

    const auto emitted{emit_test_executable(test_ctx, context, exe_file.path, {}, {})};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    const mock_argv args{exe_file.path.string()};
    return UNWRAP(spawn_child(args));
}

} // namespace ghoti::tests::helpers
