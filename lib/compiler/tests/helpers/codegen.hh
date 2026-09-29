#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "compiler/sema/analyzer.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

[[nodiscard]] auto ir_text(llvm::Module& mod) -> std::string;

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

// Links like the driver does: compiler_rt from the shipped `lib/compiler_rt` when needed
[[nodiscard]] auto default_linker_options() -> codegen::extra_linker_options;

auto emit_executable(helpers::sema_test_context&          test_ctx,
                     llvm::LLVMContext&                   context,
                     const std::filesystem::path&         output_path,
                     const codegen::target_options&       target_opts = {},
                     const codegen::optimizer_options&    opt_options = {},
                     const codegen::extra_linker_options& linker_opts = default_linker_options())
    -> stdx::result<void, codegen::diagnostic>;

auto emit_test_executable(helpers::sema_test_context&          test_ctx,
                          llvm::LLVMContext&                   context,
                          const std::filesystem::path&         output_path,
                          const codegen::target_options&       target_opts = {},
                          const codegen::optimizer_options&    opt_options = {},
                          const codegen::extra_linker_options& linker_opts =
                              default_linker_options()) -> stdx::result<void, codegen::diagnostic>;

auto emit_static_lib(helpers::sema_test_context&       test_ctx,
                     llvm::LLVMContext&                context,
                     const std::filesystem::path&      output_path,
                     const codegen::target_options&    target_opts = {},
                     const codegen::optimizer_options& opt_options = {})
    -> stdx::result<void, codegen::diagnostic>;

auto emit_dynamic_lib(helpers::sema_test_context&       test_ctx,
                      llvm::LLVMContext&                context,
                      const std::filesystem::path&      output_path,
                      const codegen::target_options&    target_opts = {},
                      const codegen::optimizer_options& opt_options = {})
    -> stdx::result<void, codegen::diagnostic>;

// The names a shared library, DLL, or dylib exports, without the target's global prefix
[[nodiscard]] auto exported_symbols(const std::filesystem::path&   library,
                                    const codegen::target_options& target_opts = {})
    -> std::vector<std::string>;

[[nodiscard]] auto
compile_and_run(std::string_view                     source,
                const std::vector<mock_file>&        imports     = {},
                const codegen::extra_linker_options& linker_opts = default_linker_options()) -> u32;

// Every symbol an archive's index says it defines, without the target's global prefix
[[nodiscard]] auto archive_definitions(const std::filesystem::path&   archive,
                                       const codegen::target_options& target_opts = {})
    -> std::vector<std::string>;

// The runtime libcalls `source`'s program needs that the shipped compiler_rt doesn't define yet,
// so a test can `SKIP` until the routines exist
[[nodiscard]] auto missing_builtins(std::string_view               source,
                                    const codegen::target_options& target_opts = {})
    -> std::vector<std::string>;

struct run_output {
    u32         exit_code{};
    std::string out;
    std::string err;
};

[[nodiscard]] auto compile_and_run_captured(std::string_view              source,
                                            const std::vector<mock_file>& imports = {})
    -> run_output;

[[nodiscard]] auto compile_and_run_tests(std::string_view                source,
                                         const std::vector<mock_file>&   imports    = {},
                                         const std::vector<std::string>& extra_args = {}) -> u32;

} // namespace ghoti::tests::helpers
