#pragma once

#include <string_view>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::codegen {

class llvm_lowering {
  public:
    explicit llvm_lowering(llvm::LLVMContext& context,
                           std::string_view   module_name = "ghoti_module") noexcept;
    ~llvm_lowering() = default;
    MAKE_PINNED(llvm_lowering);

    [[nodiscard]] auto context(this auto&& self) noexcept -> auto& { return self.context_; }
    [[nodiscard]] auto module(this auto&& self) noexcept -> auto& { return *self.llvm_module_; }
    [[nodiscard]] auto builder() noexcept -> llvm::IRBuilder<>& { return builder_; }
    [[nodiscard]] auto types() noexcept -> type_translator& { return types_; }

    auto clear_locals() noexcept -> void { locals_.clear(); }
    auto set_local(gir::local_id id, llvm::Value* val) noexcept -> void { locals_[id] = val; }
    [[nodiscard]] auto get_local_opt(gir::local_id id) const noexcept
        -> stdx::option<llvm::Value*> {
        if (const auto it{locals_.find(id)}; it != locals_.end()) { return it->second; }
        return stdx::none;
    }

    auto lower_value(const gir::value& val, const sema::type* expected_type = nullptr)
        -> llvm::Value*;
    auto lower_instruction(const gir::instruction& inst) -> void;

    auto emit_binary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_unary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_comparison(const gir::instruction& inst) -> llvm::Value*;
    auto emit_cast(const gir::instruction& inst) -> llvm::Value*;
    auto emit_const(const gir::instruction& inst) -> llvm::Value*;

  private:
    llvm::LLVMContext&                                        context_;
    stdx::box<llvm::Module>                                   llvm_module_;
    llvm::IRBuilder<>                                         builder_;
    type_translator                                           types_;
    ankerl::unordered_dense::map<gir::local_id, llvm::Value*> locals_;
};

} // namespace ghoti::codegen
