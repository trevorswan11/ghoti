#pragma once

#include <filesystem>
#include <ostream>

#include <stdx/arena.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/error.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"

namespace llvm {

class LLVMContext;
class Module;

} // namespace llvm

namespace ghoti {

namespace gir { class module; }                 // namespace gir
namespace codegen { struct optimizer_options; } // namespace codegen

namespace sema {

// The manager for all steps of semantic analysis.
class analyzer {
  public:
    explicit analyzer(mod::module_manager& modules,
                      std::ostream&        error_stream,
                      stdx::option<bool>   in_terminal) noexcept
        : modules_{modules}, pool_{arena_}, error_stream_{error_stream}, in_terminal_{in_terminal},
          ctx_{modules_,
               registry_,
               pool_,
               generic_functions_,
               instantiation_cache_,
               arena_,
               diagnostics{in_terminal_},
               error_stream_} {}
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
    auto emit_gir(mod::module& module) -> gir::module;
    auto check_types(gir::module& gir_module, mod::module& ast_module) -> mod::module_state;
    auto emit_llvm(gir::module&                      gir_module,
                   llvm::LLVMContext&                context,
                   const codegen::optimizer_options& options)
        -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic>;

  private:
    auto optimize_llvm(llvm::Module& module, const codegen::optimizer_options& options)
        -> stdx::result<void, codegen::diagnostic>;

  private:
    mod::module_manager&        modules_;
    arena_alloc                 arena_;
    symbol_table_registry       registry_;
    type_pool                   pool_;
    generic_function_registry   generic_functions_;
    generic_instantiation_cache instantiation_cache_;
    std::ostream&               error_stream_;
    stdx::option<bool>          in_terminal_;

    context ctx_;
};

} // namespace sema

} // namespace ghoti
