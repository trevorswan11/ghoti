#pragma once

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Target/TargetMachine.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/opt_level.hh"
#include "compiler/sema/error.hh"

namespace ghoti::codegen {

class llvm_optimizer {
  public:
    explicit llvm_optimizer(llvm::LLVMContext& context) noexcept : context_{context} {}
    ~llvm_optimizer() = default;
    MAKE_PINNED(llvm_optimizer);

    auto optimize(llvm::Module& module, const optimizer_options& options)
        -> stdx::result<void, sema::diagnostic>;

  private:
    llvm::LLVMContext& context_;
};

} // namespace ghoti::codegen
