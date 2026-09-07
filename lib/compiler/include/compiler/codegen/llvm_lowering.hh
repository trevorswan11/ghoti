#pragma once

#include <string>
#include <string_view>

#include <ankerl/unordered_dense.h>
#include <gsl/span>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/const_value.hh"
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
    // `test` block in declaration order. `recover_args` passes an empty `[][:0]u8` to `test_runner`
    // instead when Win32 sysroot could not be loaded
    auto lower_test_executable(const gir::module&             gir_mod,
                               stdx::option<std::string_view> user_runner_name = stdx::none,
                               bool recover_args = true) -> stdx::box<llvm::Module>;
    auto emit_test_entry_wrapper(const gir::module& gir_mod, bool recover_args = true)
        -> llvm::Function*;

    [[nodiscard]] static auto to_ir_string(const llvm::Module& mod) -> std::string;

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
    // Emits one private constant `[N x ptr]` global per `(I, T)` `&dyn I` coercion in the module.
    auto lower_dyn_vtables(const gir::module& gir_mod) -> void;
    auto const_to_llvm(const gir::const_value& cv, llvm::Type* ty) -> llvm::Constant*;

    // Populates `reserved_symbols_` with every explicit `extern`/`export` link name in the module
    auto ensure_reserved_symbols() -> void;
    // A private, collision-free `__ghoti.<name>` symbol for an internal definition
    [[nodiscard]] auto private_symbol_name(std::string_view name) const -> std::string;
    auto               resolve_named_function(std::string_view ghoti_name) -> llvm::Function*;

    auto emit_alloca(const gir::instruction& inst) -> llvm::Value*;
    auto emit_load(const gir::instruction& inst) -> llvm::Value*;
    auto emit_store(const gir::instruction& inst) -> void;
    auto emit_get_element_ptr(const gir::instruction& inst) -> llvm::Value*;
    auto emit_address_of(const gir::instruction& inst) -> llvm::Value*;
    auto emit_deref(const gir::instruction& inst) -> llvm::Value*;
    auto emit_global_addr(const gir::instruction& inst) -> llvm::Value*;

    auto emit_binary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_unary(const gir::instruction& inst) -> llvm::Value*;
    auto emit_comparison(const gir::instruction& inst) -> llvm::Value*;
    auto emit_cast(const gir::instruction& inst) -> llvm::Value*;
    auto emit_const(const gir::instruction& inst) -> llvm::Value*;

    auto emit_call(const gir::instruction& inst) -> llvm::Value*;
    auto emit_builtin_call(const gir::instruction& inst) -> llvm::Value*;
    auto emit_inline_asm(const gir::instruction& inst) -> llvm::Value*;

    auto emit_ret(const gir::instruction& inst) -> void;
    auto emit_goto(const gir::instruction& inst) -> void;
    auto emit_cond_goto(const gir::instruction& inst) -> void;
    auto emit_unreachable() -> void;

    // Builds the `[][:0]u8` argv value (a `{ ptr, len }` slice of `{ ptr, len }`
    // elements) for an entry function
    auto emit_argv_slice(llvm::Function* entry_fn, bool want_real_args) -> llvm::Value*;

    // Emits a freestanding ELF `_start` (Linux, no crt/libc)
    auto emit_freestanding_start(llvm::Function* main_fn) -> void;

    // The `__chkstk` / `___chkstk_ms` symbol name the x86 backend probes with on this
    // target, or `none` when no synthesized stack probe is needed
    [[nodiscard]] auto windows_stack_probe_symbol() const -> stdx::option<std::string_view>;
    // Synthesizes a weak, runtime-free stack-probe routine when targeting x86-64 Windows
    auto maybe_emit_windows_stack_probe() -> void;

    // Provide `@memcpy`/`@memset`/`@memmove` overrides to llvm intrinsics w/o libc
    auto maybe_emit_mem_intrinsic_fallbacks() -> void;

    auto get_or_create_test_failed_flag() -> llvm::GlobalVariable*;
    auto get_or_create_test_skipped_flag() -> llvm::GlobalVariable*;
    auto define_test_take_skipped() -> void;

    // Calls a weak `builtin` context handler `handler(msg, file, line, column)`. `order`
    // lists which instruction operand feeds each of those four parameters
    auto emit_context_handler_call(const gir::instruction&   inst,
                                   std::string_view          handler_name,
                                   gsl::span<const usize, 4> order) -> void;

    auto emit_lowered_panic(std::string_view message, const gir::instruction& inst) -> void;
    auto emit_arith_guard(llvm::Value* bad, std::string_view message, const gir::instruction& inst)
        -> void;
    auto emit_checked_arith(const gir::instruction& inst,
                            llvm::Value*            lhs,
                            llvm::Value*            rhs,
                            bool                    is_signed) -> llvm::Value*;

    [[nodiscard]] constexpr auto mem_fallbacks_used() const noexcept -> bool {
        return memcpy_used_ || memmove_used_ || memset_used_;
    }

  private:
    llvm::LLVMContext&                                                 context_;
    stdx::box<llvm::Module>                                            llvm_module_;
    llvm::IRBuilder<>                                                  builder_;
    type_translator                                                    types_;
    ankerl::unordered_dense::map<gir::local_id, llvm::Value*>          locals_;
    ankerl::unordered_dense::map<gir::segment_id, llvm::BasicBlock*>   segment_blocks_;
    ankerl::unordered_dense::map<std::string_view, llvm::GlobalValue*> globals_;
    ankerl::unordered_dense::set<std::string>                          reserved_symbols_;
    bool                             reserved_symbols_built_{false};
    bool                             is_executable_{false};
    bool                             memcpy_used_{false};
    bool                             memmove_used_{false};
    bool                             memset_used_{false};
    stdx::option<const gir::module&> gir_module_;
    std::string                      user_main_name_{"main"};
};

} // namespace ghoti::codegen
