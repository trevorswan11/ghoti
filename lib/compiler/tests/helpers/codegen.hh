#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

auto emit_llvm_ir(helpers::sema_test_context&       test_ctx,
                  llvm::LLVMContext&                context,
                  const codegen::optimizer_options& options = {})
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;

auto emit_llvm_ir_executable(helpers::sema_test_context&       test_ctx,
                             llvm::LLVMContext&                context,
                             const codegen::optimizer_options& options        = {},
                             std::string_view                  user_main_name = "main")
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;

auto emit_object(helpers::sema_test_context&       test_ctx,
                 llvm::LLVMContext&                context,
                 const std::filesystem::path&      output_path,
                 const codegen::target_options&    target_opts = {},
                 const codegen::optimizer_options& opt_options = {})
    -> stdx::result<void, codegen::diagnostic>;

auto emit_executable(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts = {},
                     const codegen::optimizer_options& opt_options = {})
    -> stdx::result<void, codegen::diagnostic>;

auto emit_test_executable(helpers::sema_test_context&       test_ctx,
                          llvm::LLVMContext&                context,
                          const std::filesystem::path&      output_path,
                          const codegen::target_options&    target_opts      = {},
                          const codegen::optimizer_options& opt_options      = {},
                          std::string_view                  user_runner_name = "")
    -> stdx::result<void, codegen::diagnostic>;

auto emit_static_lib(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts = {},
                     const codegen::optimizer_options& opt_options = {})
    -> stdx::result<void, codegen::diagnostic>;

[[nodiscard]] auto compile_and_run(std::string_view              source,
                                   const std::vector<mock_file>& imports = {}) -> u32;

[[nodiscard]] auto compile_and_run_tests(std::string_view              source,
                                         const std::vector<mock_file>& imports     = {},
                                         std::string_view              runner_name = "") -> u32;

} // namespace ghoti::tests::helpers
