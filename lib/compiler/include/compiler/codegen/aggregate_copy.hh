#pragma once

#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ghoti::codegen {

// Moves whole aggregates between values and memory as byte copies wherever it can, since
// instruction selection expands a first-class aggregate into one value per element
class aggregate_copier {
  public:
    explicit aggregate_copier(llvm::Module& module)
        : module_{module}, layout_{module.getDataLayout()} {}

    auto copy_bytes(llvm::IRBuilder<>& builder, llvm::Value* dest, llvm::Value* src, llvm::Type* ty)
        -> void;

    // Where `value`'s bytes can be copied from at `at`: a spilled constant, or the address of a
    // load whose source nothing has overwritten since
    [[nodiscard]] auto source_address(llvm::Value* value, const llvm::Instruction* at)
        -> llvm::Value*;

    static auto erase_if_dead(llvm::Value* value) -> void;

    [[nodiscard]] static auto entry_alloca(llvm::Function& fn, llvm::Type* ty) -> llvm::AllocaInst*;

    // Uses of a by-value parameter that now arrives by pointer read through that pointer
    auto redirect_indirect_param(llvm::Argument& old_arg, llvm::Argument& pointer, llvm::Type* ty)
        -> void;

    // Every `ret value` writes through `sret` and returns nothing
    auto redirect_returns(llvm::Function& fn, llvm::Value* sret, llvm::Type* ty) -> void;

    // Copies a by-value argument into its caller-owned slot
    auto
    materialize_argument(llvm::Value* arg, llvm::Value* slot, llvm::Type* ty, llvm::CallInst& call)
        -> void;

    // Uses of a call's by-value result read the `sret` slot it now lands in
    auto redirect_sret_result(llvm::CallInst& call, llvm::Value* slot, llvm::Type* ty) -> void;

    [[nodiscard]] auto used_memcpy() const noexcept -> bool { return memcpy_used_; }

  private:
    [[nodiscard]] auto spill_constant(llvm::Constant* constant) -> llvm::GlobalVariable*;

  private:
    llvm::Module&           module_;
    const llvm::DataLayout& layout_;
    bool                    memcpy_used_{false};
};

} // namespace ghoti::codegen
