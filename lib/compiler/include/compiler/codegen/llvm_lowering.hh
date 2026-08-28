#pragma once

#include <string_view>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/sema/type.hh"

namespace ghoti::codegen {

class llvm_lowering {
  public:
    explicit llvm_lowering(llvm::LLVMContext& context,
                           std::string_view   module_name = "ghoti_module") noexcept;
    ~llvm_lowering() = default;
    MAKE_PINNED(llvm_lowering);

    auto lower(const gir::module& gir_mod) -> stdx::box<llvm::Module>;
    auto lower_executable(const gir::module& gir_mod, std::string_view user_main_name = "main")
        -> stdx::box<llvm::Module>;
    auto emit_main_entry_wrapper(std::string_view user_main_name = "main") -> llvm::Function*;

    // Lowers every function/global as usual, but synthesizes an entry point that calls each
    // `test` block in declaration order
    auto lower_test_executable(const gir::module& gir_mod, std::string_view user_runner_name = "")
        -> stdx::box<llvm::Module>;
    auto emit_test_entry_wrapper(const gir::module& gir_mod) -> llvm::Function*;

    [[nodiscard]] auto context(this auto&& self) noexcept -> auto& { return self.context_; }
    [[nodiscard]] auto module(this auto&& self) noexcept -> auto& { return *self.llvm_module_; }
    [[nodiscard]] auto builder() noexcept -> llvm::IRBuilder<>& { return builder_; }
    [[nodiscard]] auto types() noexcept -> type_translator& { return types_; }

    auto set_local(gir::local_id id, llvm::Value* val) noexcept -> void { locals_[id] = val; }
    [[nodiscard]] auto get_local_opt(gir::local_id id) const noexcept
        -> stdx::option<llvm::Value*> {
        if (const auto it{locals_.find(id)}; it != locals_.end()) { return it->second; }
        return stdx::none;
    }

    auto clear_locals() noexcept -> void {
        locals_.clear();
        segment_blocks_.clear();
    }

    auto lower_value(const gir::value& val, const sema::type* expected_type = nullptr)
        -> llvm::Value*;
    auto lower_instruction(const gir::instruction& inst) -> void;

  private:
    auto declare_function(const gir::function& fn) -> llvm::Function*;
    auto lower_function(const gir::function& fn) -> llvm::Function*;
    auto lower_global(const gir::global_decl& g) -> llvm::GlobalVariable*;

    auto emit_alloca(const gir::instruction& inst) -> llvm::Value*;
    auto emit_load(const gir::instruction& inst) -> llvm::Value*;
    auto emit_store(const gir::instruction& inst) -> void;
    auto emit_get_element_ptr(const gir::instruction& inst) -> llvm::Value*;
    auto emit_address_of(const gir::instruction& inst) -> llvm::Value*;
    auto emit_deref(const gir::instruction& inst) -> llvm::Value*;

    auto emit_binary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_unary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_comparison(const gir::instruction& inst) -> llvm::Value*;
    auto emit_cast(const gir::instruction& inst) -> llvm::Value*;
    auto emit_const(const gir::instruction& inst) -> llvm::Value*;

    auto emit_call(const gir::instruction& inst) -> llvm::Value*;
    auto emit_builtin_call(const gir::instruction& inst) -> llvm::Value*;

    auto emit_ret(const gir::instruction& inst) -> void;
    auto emit_goto(const gir::instruction& inst) -> void;
    auto emit_cond_goto(const gir::instruction& inst) -> void;
    auto emit_unreachable() -> void;

    auto get_or_create_test_failed_flag() -> llvm::GlobalVariable*;

  private:
    llvm::LLVMContext&                                                 context_;
    stdx::box<llvm::Module>                                            llvm_module_;
    llvm::IRBuilder<>                                                  builder_;
    type_translator                                                    types_;
    ankerl::unordered_dense::map<gir::local_id, llvm::Value*>          locals_;
    ankerl::unordered_dense::map<gir::segment_id, llvm::BasicBlock*>   segment_blocks_;
    ankerl::unordered_dense::map<std::string_view, llvm::GlobalValue*> globals_;
    bool                                                               is_executable_{false};
    std::string                                                        user_main_name_{"main"};
};

} // namespace ghoti::codegen
