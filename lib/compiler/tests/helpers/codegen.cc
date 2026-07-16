#include <llvm/IR/Module.h>
#include <llvm/Support/ManagedStatic.h>
#include <stdx/memory.hh>
#include <stdx/result.hh>

#include "compiler/gir/module.hh" // IWYU pragma: keep
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

llvm_test_scope::~llvm_test_scope() { llvm::llvm_shutdown(); }

auto emit_llvm(helpers::sema_test_context&       test_ctx,
               llvm::LLVMContext&                context,
               const codegen::optimizer_options& options)
    -> stdx::result<stdx::box<llvm::Module>, sema::diagnostic> {
    if (test_ctx.root_mod.is_poisoned()) {
        return make_sema_err("Module is poisoned", sema::error::MODULE_LOAD_ERROR);
    }

    auto gir_mod{test_ctx.analyzer.emit_gir(test_ctx.root_mod)};
    if (test_ctx.root_mod.is_poisoned()) {
        return make_sema_err("Module is poisoned during GIR emission",
                             sema::error::MODULE_LOAD_ERROR);
    }
    return test_ctx.analyzer.emit_llvm(gir_mod, context, options);
}

} // namespace ghoti::tests::helpers
