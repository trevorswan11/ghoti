#pragma once

#include <stdx/memory.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/error.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

struct llvm_test_scope {
    llvm_test_scope() = default;
    ~llvm_test_scope();
    MAKE_PINNED(llvm_test_scope);
};

auto emit_llvm(helpers::sema_test_context&       test_ctx,
               llvm::LLVMContext&                context,
               const codegen::optimizer_options& options = {})
    -> stdx::result<stdx::box<llvm::Module>, sema::diagnostic>;

} // namespace ghoti::tests::helpers
