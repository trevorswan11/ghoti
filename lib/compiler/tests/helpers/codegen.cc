#include <llvm/IR/Module.h>
#include <llvm/Support/ManagedStatic.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/gir/module.hh" // IWYU pragma: keep
#include "compiler/sema/analyzer.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

llvm_test_scope::~llvm_test_scope() { llvm::llvm_shutdown(); }

auto emit_llvm(helpers::sema_test_context&       test_ctx,
               llvm::LLVMContext&                context,
               const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, codegen::diagnostic> {
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned", codegen::error::MODULE_LOAD_ERROR);
    }

    auto gir_mod{test_ctx.analyzer.emit_gir(test_ctx.root_mod)};
    if (test_ctx.root_mod.is_poisoned()) {
        return codegen::make_codegen_err("Module is poisoned during GIR emission",
                                         codegen::error::MODULE_LOAD_ERROR);
    }
    return test_ctx.analyzer.emit_llvm(gir_mod, context, options);
}

} // namespace ghoti::tests::helpers
