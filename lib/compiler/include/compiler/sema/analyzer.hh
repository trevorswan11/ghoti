#pragma once

#include <filesystem>
#include <ostream>
#include <string>
#include <string_view>

#include <stdx/arena.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/codegen/error.hh"
#include "compiler/codegen/linker.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"

namespace llvm {

class LLVMContext;
class Module;

} // namespace llvm

namespace ghoti {

namespace codegen {

struct optimizer_options;
struct target_options;

} // namespace codegen

namespace sema {

// The manager for all steps of semantic analysis.
class analyzer {
  public:
    explicit analyzer(mod::module_manager&    modules,
                      std::ostream&           error_stream,
                      stdx::option<bool>      in_terminal,
                      codegen::target_options target_opts            = {},
                      bool                    tolerate_syntax_errors = false,
                      bool                    runtime_safety         = true) noexcept
        : modules_{modules}, arena_{modules.arena()}, pool_{arena_}, error_stream_{error_stream},
          in_terminal_{in_terminal}, tolerate_syntax_errors_{tolerate_syntax_errors},
          ctx_{modules_,
               registry_,
               pool_,
               generic_functions_,
               instantiation_cache_,
               impl_registry_,
               arena_,
               diagnostics{in_terminal_},
               error_stream_,
               std::move(target_opts)} {
        ctx_.runtime_safety = runtime_safety;
    }
    ~analyzer() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(analyzer);

    // Runs the entire sema pipeline
    auto analyze(const std::filesystem::path& entry_path) -> stdx::result<void, diagnostic>;

    [[nodiscard]] auto get_table(this auto&& self, usize idx) -> auto& {
        return self.registry_.get(idx);
    }

    [[nodiscard]] auto get_table_opt(this auto&& self, usize idx) noexcept {
        return self.registry_.get_opt(idx);
    }

    MAKE_DEDUCING_GETTER(arena);
    MAKE_DEDUCING_GETTER(registry);
    MAKE_DEDUCING_GETTER(pool);
    MAKE_DEDUCING_GETTER(generic_functions);
    MAKE_DEDUCING_GETTER(instantiation_cache);
    MAKE_DEDUCING_GETTER(ctx);

    [[nodiscard]] auto get_prelude_index_opt() const noexcept -> stdx::opt_size {
        return ctx_.prelude_index;
    }

    auto collect_symbols(mod::module& module) -> mod::module_state;
    auto resolve_types(mod::module& module) -> mod::module_state;
    auto emit_gir(mod::module& module, bool for_test_executable = false) -> gir::module;
    auto check_types(gir::module& gir_module, mod::module& ast_module) -> mod::module_state;
    [[nodiscard]] auto validate_main_entry(const mod::module& root_module) const
        -> stdx::result<void, diagnostic>;

    // Checks a user-defined `test_runner` override has sig `fn(args: [][:0]u8, tests: []Test): i32`
    // A no-op when the module relies on the `weak` `builtin` default.
    [[nodiscard]] auto validate_test_entry(const mod::module& root_module) const
        -> stdx::result<void, diagnostic>;

    [[nodiscard]] auto emit_llvm_ir(gir::module&                      gir_module,
                                    llvm::LLVMContext&                context,
                                    const codegen::optimizer_options& options)
        -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;

    // Lowers `gir_module` and returns the resulting module's textual LLVM IR.
    [[nodiscard]] auto emit_llvm_ir_text(gir::module&                      gir_module,
                                         const codegen::optimizer_options& options)
        -> stdx::result<std::string, codegen::diagnostic>;
    [[nodiscard]] auto emit_llvm_ir_executable(gir::module&                      gir_module,
                                               llvm::LLVMContext&                context,
                                               const codegen::optimizer_options& options,
                                               std::string_view user_main_name = "main")
        -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;
    [[nodiscard]] auto emit_llvm_ir_test_executable(gir::module&                      gir_module,
                                                    llvm::LLVMContext&                context,
                                                    const codegen::optimizer_options& options)
        -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;

    [[nodiscard]] auto emit_object(gir::module&                      gir_module,
                                   const codegen::target_options&    target_opts,
                                   const codegen::optimizer_options& opt_options,
                                   const std::filesystem::path&      output_path)
        -> stdx::result<void, codegen::diagnostic>;
    [[nodiscard]] auto emit_object(gir::module&                      gir_module,
                                   llvm::LLVMContext&                context,
                                   const codegen::target_options&    target_opts,
                                   const codegen::optimizer_options& opt_options,
                                   const std::filesystem::path&      output_path)
        -> stdx::result<void, codegen::diagnostic>;

    [[nodiscard]] auto emit_executable(gir::module&                         gir_module,
                                       const codegen::target_options&       target_opts,
                                       const codegen::optimizer_options&    opt_options,
                                       const std::filesystem::path&         output_path,
                                       const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;
    [[nodiscard]] auto emit_executable(gir::module&                         gir_module,
                                       llvm::LLVMContext&                   context,
                                       const codegen::target_options&       target_opts,
                                       const codegen::optimizer_options&    opt_options,
                                       const std::filesystem::path&         output_path,
                                       const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;

    [[nodiscard]] auto emit_test_executable(gir::module&                         gir_module,
                                            const codegen::target_options&       target_opts,
                                            const codegen::optimizer_options&    opt_options,
                                            const std::filesystem::path&         output_path,
                                            const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;
    [[nodiscard]] auto emit_test_executable(gir::module&                         gir_module,
                                            llvm::LLVMContext&                   context,
                                            const codegen::target_options&       target_opts,
                                            const codegen::optimizer_options&    opt_options,
                                            const std::filesystem::path&         output_path,
                                            const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;

    [[nodiscard]] auto emit_static_library(gir::module&                         gir_module,
                                           const codegen::target_options&       target_opts,
                                           const codegen::optimizer_options&    opt_options,
                                           const std::filesystem::path&         output_path,
                                           const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;
    [[nodiscard]] auto emit_static_library(gir::module&                         gir_module,
                                           llvm::LLVMContext&                   context,
                                           const codegen::target_options&       target_opts,
                                           const codegen::optimizer_options&    opt_options,
                                           const std::filesystem::path&         output_path,
                                           const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;

    [[nodiscard]] auto emit_dynamic_library(gir::module&                         gir_module,
                                            const codegen::target_options&       target_opts,
                                            const codegen::optimizer_options&    opt_options,
                                            const std::filesystem::path&         output_path,
                                            const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;
    [[nodiscard]] auto emit_dynamic_library(gir::module&                         gir_module,
                                            llvm::LLVMContext&                   context,
                                            const codegen::target_options&       target_opts,
                                            const codegen::optimizer_options&    opt_options,
                                            const std::filesystem::path&         output_path,
                                            const codegen::extra_linker_options& linker_opts = {})
        -> stdx::result<void, codegen::diagnostic>;

  private:
    auto optimize_llvm(llvm::Module& module, const codegen::optimizer_options& options)
        -> stdx::result<void, codegen::diagnostic>;

  private:
    mod::module_manager&        modules_;
    ghoti::arena&               arena_;
    symbol_table_registry       registry_;
    type_pool                   pool_;
    generic_function_registry   generic_functions_;
    generic_instantiation_cache instantiation_cache_;
    impl_registry               impl_registry_{arena_};
    std::ostream&               error_stream_;
    stdx::option<bool>          in_terminal_;
    bool                        tolerate_syntax_errors_;

    context ctx_;
};

} // namespace sema

} // namespace ghoti
