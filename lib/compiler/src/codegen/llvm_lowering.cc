#include "compiler/codegen/llvm_lowering.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/AtomicOrdering.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::codegen {

namespace {

// A `constexpr_float` materializes as `f64`; a `constexpr_int` as `i32`.
[[nodiscard]] auto materialized_is_float(sema::type_kind k) noexcept -> bool {
    return sema::is_float(k) || k == sema::type_kind::CONSTEXPR_FLOAT;
}

[[nodiscard]] auto is_float_type(const gir::instruction&    inst,
                                 stdx::option<llvm::Value&> val) noexcept -> bool {
    // Any float (or `constexpr_float`) operand makes the operation floating point.
    for (const auto& op : inst.operands) {
        if (op.type && materialized_is_float(op.type->get_kind())) { return true; }
    }
    if (!inst.operands.empty() && inst.operands[0].type &&
        sema::is_integer(inst.operands[0].type->get_kind())) {
        return false;
    }
    if (inst.type) { return materialized_is_float(inst.type->get_kind()); }
    return val && val->getType()->isFloatingPointTy();
}

[[nodiscard]] auto is_signed_type(const gir::instruction& inst) noexcept -> bool {
    if (!inst.operands.empty() && inst.operands[0].type) {
        if (inst.operands[0].type->get_kind() == sema::type_kind::CONSTEXPR_INT) { return true; }
        return sema::is_signed_integer(*inst.operands[0].type);
    }
    if (inst.type) { return sema::is_signed_integer(*inst.type); }
    return false;
}

// Build an integer constant of `ty`'s width from a full 128-bit value.
[[nodiscard]] auto wide_int_constant(u128 v, llvm::Type* ty) -> llvm::Constant* {
    const std::array<u64, 2> words{static_cast<u64>(v), static_cast<u64>(v >> 64)};
    const llvm::APInt        value{128, words};
    return llvm::ConstantInt::get(ty->getContext(), value.zextOrTrunc(ty->getIntegerBitWidth()));
}

// An integer-valued constant whose GIR type is a pointer must materialize as an `inttoptr` constant
[[nodiscard]] auto int_or_ptr_constant(u128 bits, llvm::Type* ty) -> llvm::Constant* {
    if (!ty->isPointerTy()) { return wide_int_constant(bits, ty); }
    if (bits == 0) { return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty)); }
    auto* int_ty{llvm::Type::getInt64Ty(ty->getContext())};
    return llvm::ConstantExpr::getIntToPtr(wide_int_constant(bits, int_ty), ty);
}

[[nodiscard]] auto to_llvm_callconv(ast::calling_convention conv) noexcept
    -> llvm::CallingConv::ID {
    switch (conv) {
    case ast::calling_convention::C:            return llvm::CallingConv::C;
    case ast::calling_convention::SYSV:         return llvm::CallingConv::X86_64_SysV;
    case ast::calling_convention::WIN64:        return llvm::CallingConv::Win64;
    case ast::calling_convention::X86_STDCALL:  return llvm::CallingConv::X86_StdCall;
    case ast::calling_convention::X86_FASTCALL: return llvm::CallingConv::X86_FastCall;
    case ast::calling_convention::AAPCS:        return llvm::CallingConv::ARM_AAPCS;
    default:                                    return llvm::CallingConv::C;
    }
}

enum memory_order_t : u8 {
    mo_relaxed = 0,
    mo_acquire = 1,
    mo_release = 2,
    mo_acq_rel = 3,
    mo_seq_cst = 4,
};

// `ord` is a `MemoryOrder` enum ordinal as folded from the atomic builtin
[[nodiscard]] auto to_llvm_ordering(u8 ord) noexcept -> llvm::AtomicOrdering {
    switch (ord) {
    case mo_relaxed: return llvm::AtomicOrdering::Monotonic;
    case mo_acquire: return llvm::AtomicOrdering::Acquire;
    case mo_release: return llvm::AtomicOrdering::Release;
    case mo_acq_rel: return llvm::AtomicOrdering::AcquireRelease;
    case mo_seq_cst:
    default:         return llvm::AtomicOrdering::SequentiallyConsistent;
    }
}

// Weak enum mirroring builtin.gh.inc
enum atomic_rmw_op_t : u8 {
    rmw_xchg = 0,
    rmw_add  = 1,
    rmw_sub  = 2,
    rmw_band = 3,
    rmw_nand = 4,
    rmw_bor  = 5,
    rmw_bxor = 6,
    rmw_max  = 7,
    rmw_min  = 8,
    rmw_umax = 9,
    rmw_umin = 10,
};

// `op` is an `AtomicRmwOp` enum ordinal
[[nodiscard]] auto to_llvm_rmw_op(u8 op) noexcept -> llvm::AtomicRMWInst::BinOp {
    using Op = llvm::AtomicRMWInst::BinOp;
    switch (op) {
    case rmw_xchg: return Op::Xchg;
    case rmw_add:  return Op::Add;
    case rmw_sub:  return Op::Sub;
    case rmw_band: return Op::And;
    case rmw_nand: return Op::Nand;
    case rmw_bor:  return Op::Or;
    case rmw_bxor: return Op::Xor;
    case rmw_max:  return Op::Max;
    case rmw_min:  return Op::Min;
    case rmw_umax: return Op::UMax;
    case rmw_umin:
    default:       return Op::UMin;
    }
}

} // namespace

llvm_lowering::llvm_lowering(llvm::LLVMContext& context, std::string_view module_name) noexcept
    : context_{context}, llvm_module_{stdx::make_box<llvm::Module>(module_name, context_)},
      builder_{context_}, types_{context_, *llvm_module_} {}

auto llvm_lowering::to_ir_string(const llvm::Module& mod) -> std::string {
    PROFILE_FUNCTION();
    std::string              out;
    llvm::raw_string_ostream os{out};
    mod.print(os, nullptr);
    return out;
}

auto llvm_lowering::lower(const gir::module& gir_mod) -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    gir_module_.emplace(gir_mod);
    {
        PROFILE_SCOPE("llvm_lowering: declare functions");
        for (const auto* fn : gir_mod.get_functions()) { declare_function(*fn); }
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower globals");
        for (const auto* global : gir_mod.get_globals()) { lower_global(*global); }
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower functions");
        lower_dyn_vtables(gir_mod);
        for (const auto* fn : gir_mod.get_functions()) { lower_function(*fn); }
    }
    maybe_emit_windows_stack_probe();
    return std::move(llvm_module_);
}

auto llvm_lowering::lower_dyn_vtables(const gir::module& gir_mod) -> void {
    PROFILE_FUNCTION();
    auto* ptr_ty{types_.get_ptr_ty()};
    for (const auto& vt : gir_mod.get_ast_module().dyn_vtables) {
        if (llvm_module_->getNamedGlobal(vt.symbol)) { continue; }
        std::vector<llvm::Constant*> slots;
        slots.reserve(vt.slots.size());
        for (const auto& name : vt.slots) {
            auto* fn{llvm_module_->getFunction(name)};
            slots.emplace_back(fn ? llvm::cast<llvm::Constant>(fn)
                                  : llvm::ConstantPointerNull::get(ptr_ty));
        }

        auto* arr_ty{llvm::ArrayType::get(ptr_ty, slots.size())};
        new llvm::GlobalVariable(*llvm_module_,
                                 arr_ty,
                                 true,
                                 llvm::GlobalValue::InternalLinkage,
                                 llvm::ConstantArray::get(arr_ty, slots),
                                 vt.symbol);
    }
}

auto llvm_lowering::lower_executable(const gir::module& gir_mod, std::string_view user_main_name)
    -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    is_executable_  = true;
    user_main_name_ = user_main_name;
    gir_module_.emplace(gir_mod);
    {
        PROFILE_SCOPE("llvm_lowering: declare functions");
        for (const auto* fn : gir_mod.get_functions()) { declare_function(*fn); }
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower globals");
        for (const auto* global : gir_mod.get_globals()) { lower_global(*global); }
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower functions");
        lower_dyn_vtables(gir_mod);
        for (const auto* fn : gir_mod.get_functions()) { lower_function(*fn); }
    }
    emit_main_entry_wrapper(user_main_name_);
    maybe_emit_windows_stack_probe();
    return std::move(llvm_module_);
}

auto llvm_lowering::emit_main_entry_wrapper(std::string_view user_main_name) -> llvm::Function* {
    PROFILE_FUNCTION();
    auto* user_fn{llvm_module_->getFunction("_ghoti_main")};
    if (!user_fn) { user_fn = llvm_module_->getFunction(user_main_name); }
    ASSERT(user_fn, "User main function not found in LLVM module");

    auto* main_fn_ty{llvm::FunctionType::get(
        types_.get_int32_ty(), {types_.get_int32_ty(), types_.get_ptr_ty()}, false)};
    auto* main_fn{llvm::Function::Create(
        main_fn_ty, llvm::Function::ExternalLinkage, "main", llvm_module_.get())};
    main_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    main_fn->addFnAttr("no-builtins");
    main_fn->addFnAttr("no-stack-arg-probe", "true");

    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (triple.isWindowsGNUEnvironment() && !llvm_module_->getFunction("__main")) {
        auto*             void_ty{llvm::Type::getVoidTy(context_)};
        auto*             dummy_main_ty{llvm::FunctionType::get(void_ty, false)};
        auto*             dummy_main{llvm::Function::Create(
            dummy_main_ty, llvm::Function::ExternalLinkage, "__main", llvm_module_.get())};
        auto*             dummy_bb{llvm::BasicBlock::Create(context_, "entry", dummy_main)};
        llvm::IRBuilder<> dummy_builder{dummy_bb};
        dummy_builder.CreateRetVoid();
        llvm::appendToUsed(*llvm_module_, {dummy_main});
    }

    auto* entry_bb{llvm::BasicBlock::Create(context_, "entry", main_fn)};
    builder_.SetInsertPoint(entry_bb);

    // Nothing reads args unless user main takes the `[][:0]u8` parameter.
    const bool takes_arg{user_fn->getFunctionType()->getNumParams() == 1};
    auto*      outer_val{emit_argv_slice(main_fn, takes_arg)};

    if (user_fn->getReturnType()->isVoidTy()) {
        if (takes_arg) {
            builder_.CreateCall(user_fn, {outer_val});
        } else {
            builder_.CreateCall(user_fn, {});
        }
        builder_.CreateRet(builder_.getInt32(0));
    } else {
        auto* ret_val{takes_arg ? builder_.CreateCall(user_fn, {outer_val}, "main.res")
                                : builder_.CreateCall(user_fn, {}, "main.res")};
        auto* ret_i32{builder_.CreateIntCast(ret_val, types_.get_int32_ty(), true, "main.res.i32")};
        builder_.CreateRet(ret_i32);
    }

    if (triple.isOSLinux()) { emit_freestanding_start(main_fn); }
    return main_fn;
}

auto llvm_lowering::emit_freestanding_start(llvm::Function* main_fn) -> void {
    PROFILE_FUNCTION();
    const llvm::Triple triple{llvm_module_->getTargetTriple()};

    std::string asm_text;
    switch (triple.getArch()) {
    case llvm::Triple::x86_64:
        asm_text = "xorl %ebp, %ebp\n\t"    // mark the outermost frame
                   "movq (%rsp), %rdi\n\t"  // argc
                   "leaq 8(%rsp), %rsi\n\t" // argv
                   "andq $$-16, %rsp\n\t"   // realign for the SysV call
                   "call main\n\t"
                   "movl %eax, %edi\n\t"  // exit code <- main()'s return
                   "movl $$231, %eax\n\t" // __NR_exit_group
                   "syscall\n\t"
                   "ud2\n\t";
        break;
    case llvm::Triple::aarch64:
        asm_text = "mov x29, xzr\n\t"
                   "mov x30, xzr\n\t"
                   "ldr x0, [sp]\n\t"   // argc (read before realigning sp)
                   "add x1, sp, #8\n\t" // argv
                   "mov x2, sp\n\t"
                   "and x2, x2, #-16\n\t"
                   "mov sp, x2\n\t"
                   "bl main\n\t"
                   "mov w8, #94\n\t" // __NR_exit_group
                   "svc #0\n\t"
                   "brk #0\n\t";
        break;
    case llvm::Triple::riscv64:
        asm_text = "li s0, 0\n\t"
                   "ld a0, 0(sp)\n\t"   // argc
                   "addi a1, sp, 8\n\t" // argv
                   "andi sp, sp, -16\n\t"
                   "call main\n\t"
                   "li a7, 94\n\t" // __NR_exit_group  (asm-generic table)
                   "ecall\n\t"
                   "unimp\n\t";
        break;
    case llvm::Triple::riscv32:
        asm_text = "li s0, 0\n\t"
                   "lw a0, 0(sp)\n\t"   // argc (32-bit word)
                   "addi a1, sp, 4\n\t" // argv
                   "andi sp, sp, -16\n\t"
                   "call main\n\t"
                   "li a7, 94\n\t" // __NR_exit_group  (asm-generic table)
                   "ecall\n\t"
                   "unimp\n\t";
        break;
    case llvm::Triple::arm:
    case llvm::Triple::thumb:
        // One template for A32 and T32. The kernel delivers an 8-byte aligned sp at
        // entry (AAPCS), so no realign is needed before the call to `main`
        asm_text = "ldr r0, [sp, #0]\n\t" // argc
                   "add r1, sp, #4\n\t"   // argv
                   "bl main\n\t"
                   "movs r7, #248\n\t" // __NR_exit_group  (ARM EABI)
                   "svc #0\n\t"
                   "udf #0\n\t";
        break;
    case llvm::Triple::loongarch64:
        // LoongArch registers are spelled `$a0` etc.; `$` is the operand sigil in
        // an LLVM asm string, so each one is escaped as `$$`.
        asm_text = "move $$fp, $$zero\n\t"
                   "ld.d $$a0, $$sp, 0\n\t"   // argc
                   "addi.d $$a1, $$sp, 8\n\t" // argv
                   "bl main\n\t"
                   "ori $$a7, $$zero, 94\n\t" // __NR_exit_group  (asm-generic table)
                   "syscall 0\n\t"
                   "break 0\n\t";
        break;
    default:
        // Linux arch with a diagnostic before lowering runs. Kept as a safe no-op.
        return;
    }

    auto* void_ty{llvm::Type::getVoidTy(context_)};
    auto* start_ty{llvm::FunctionType::get(void_ty, false)};
    auto* start_fn{llvm::Function::Create(
        start_ty, llvm::Function::ExternalLinkage, "_start", llvm_module_.get())};
    start_fn->addFnAttr(llvm::Attribute::Naked);
    start_fn->addFnAttr(llvm::Attribute::NoInline);
    start_fn->addFnAttr(llvm::Attribute::NoUnwind);
    start_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    start_fn->addFnAttr("no-stack-arg-probe", "true");

    auto* entry_bb{llvm::BasicBlock::Create(context_, "entry", start_fn)};
    builder_.SetInsertPoint(entry_bb);

    auto* asm_ty{llvm::FunctionType::get(void_ty, false)};
    auto* start_asm{
        llvm::InlineAsm::get(asm_ty, asm_text, "~{memory}", true, false, llvm::InlineAsm::AD_ATT)};
    builder_.CreateCall(start_asm, {});
    builder_.CreateUnreachable();
    llvm::appendToUsed(*llvm_module_, {start_fn, main_fn});
}

auto llvm_lowering::windows_stack_probe_symbol() const -> stdx::option<std::string_view> {
    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (!triple.isOSWindows() || triple.getArch() != llvm::Triple::x86_64) { return stdx::none; }
    return triple.isOSCygMing() ? "___chkstk_ms" : "__chkstk";
}

// Emit a weak, self-contained probe so `no-stack-arg-probe` can stay off on Windows and
// large frames grow the stack one 4 KiB page at a time. Weak + its own section: a build
// with no big frame drops it at link time (`--gc-sections` / `/opt:ref`).
auto llvm_lowering::maybe_emit_windows_stack_probe() -> void {
    const auto sym{windows_stack_probe_symbol()};
    if (!sym || llvm_module_->getFunction(*sym)) { return; }

    auto* void_ty{llvm::Type::getVoidTy(context_)};
    auto* fn_ty{llvm::FunctionType::get(void_ty, false)};
    auto* probe_fn{
        llvm::Function::Create(fn_ty, llvm::GlobalValue::WeakAnyLinkage, *sym, llvm_module_.get())};
    probe_fn->setComdat(llvm_module_->getOrInsertComdat(*sym));
    probe_fn->addFnAttr(llvm::Attribute::Naked);
    probe_fn->addFnAttr(llvm::Attribute::NoInline);
    probe_fn->addFnAttr(llvm::Attribute::NoUnwind);
    probe_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    probe_fn->addFnAttr("no-stack-arg-probe", "true");

    // Allocation size is passed in %rax; %rsp is left untouched. Every register except the saved
    // scratch is preserved. This is libgcc's `___chkstk_ms`, which also satisfies the MSVC
    // `__chkstk` contract on x64.
    static constexpr std::string_view probe_asm{R"(push %rcx
	push %rax
	cmp $$0x1000, %rax
	lea 0x18(%rsp), %rcx
	jb 2f
	1:
	sub $$0x1000, %rcx
	orq $$0x0, (%rcx)
	sub $$0x1000, %rax
	cmp $$0x1000, %rax
	ja 1b
	2:
	sub %rax, %rcx
	orq $$0x0, (%rcx)
	pop %rax
	pop %rcx
	ret
	)"};

    auto* entry_bb{llvm::BasicBlock::Create(context_, "entry", probe_fn)};
    builder_.SetInsertPoint(entry_bb);
    auto* asm_ty{llvm::FunctionType::get(void_ty, false)};
    auto* probe_asm_val{
        llvm::InlineAsm::get(asm_ty, probe_asm, "~{memory}", true, false, llvm::InlineAsm::AD_ATT)};
    builder_.CreateCall(probe_asm_val, {});
    builder_.CreateUnreachable();
}

auto llvm_lowering::emit_argv_slice(llvm::Function* entry_fn, bool want_real_args) -> llvm::Value* {
    PROFILE_FUNCTION();
    auto*              slice_ty{types_.translate_slice_type()};
    const llvm::Triple triple{llvm_module_->getTargetTriple()};

    // Windows raw PE entry point
    if (triple.isOSWindows()) {
        // BaseThreadInitThunk calls entry with no argc/argv, unlike the POSIX path below.
        llvm::Value* outer_val{nullptr};
        if (!want_real_args) {
            // Nothing reads args -- skip the WinAPI calls entirely and pass an empty slice
            auto* outer_slice{builder_.CreateAlloca(slice_ty, nullptr, "outer.slice")};
            auto* outer_data{builder_.CreateStructGEP(
                slice_ty, outer_slice, static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX), "outer.data")};
            auto* outer_len{builder_.CreateStructGEP(
                slice_ty, outer_slice, static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX), "outer.len")};
            builder_.CreateStore(llvm::ConstantPointerNull::get(types_.get_ptr_ty()), outer_data);
            builder_.CreateStore(builder_.getInt64(0), outer_len);
            outer_val = builder_.CreateLoad(slice_ty, outer_slice, "outer.val");
        } else {
            // Recover argv via raw Win32 APIs (no CRT): split into wide args, convert to UTF-8.
            auto* get_cmdline_ty{llvm::FunctionType::get(types_.get_ptr_ty(), {}, false)};
            auto  get_cmdline_fn{
                llvm_module_->getOrInsertFunction("GetCommandLineW", get_cmdline_ty)};

            auto* cmdline_to_argv_ty{llvm::FunctionType::get(
                types_.get_ptr_ty(), {types_.get_ptr_ty(), types_.get_ptr_ty()}, false)};
            auto  cmdline_to_argv_fn{
                llvm_module_->getOrInsertFunction("CommandLineToArgvW", cmdline_to_argv_ty)};

            auto* wc2mb_ty{llvm::FunctionType::get(types_.get_int32_ty(),
                                                   {types_.get_int32_ty(),
                                                    types_.get_int32_ty(),
                                                    types_.get_ptr_ty(),
                                                    types_.get_int32_ty(),
                                                    types_.get_ptr_ty(),
                                                    types_.get_int32_ty(),
                                                    types_.get_ptr_ty(),
                                                    types_.get_ptr_ty()},
                                                   false)};
            auto  wc2mb_fn{llvm_module_->getOrInsertFunction("WideCharToMultiByte", wc2mb_ty)};

            constexpr u64 max_args{16};
            constexpr u64 max_arg_bytes{1'024};
            constexpr u32 cp_utf8{65'001};

            auto* nargs_slot{builder_.CreateAlloca(types_.get_int32_ty(), nullptr, "nargs")};
            auto* cmdline{builder_.CreateCall(get_cmdline_fn, {}, "cmdline")};
            auto* wargv{builder_.CreateCall(cmdline_to_argv_fn, {cmdline, nargs_slot}, "wargv")};
            auto* nargs_i32{builder_.CreateLoad(types_.get_int32_ty(), nargs_slot, "nargs.val")};
            auto* raw_argc_i64{builder_.CreateSExt(nargs_i32, types_.get_int64_ty(), "argc.i64")};

            auto* max_args_v{builder_.getInt64(max_args)};
            auto* slice_array{builder_.CreateAlloca(slice_ty, max_args_v, "args.array")};
            auto* arg_bufs{builder_.CreateAlloca(
                types_.get_int8_ty(), builder_.getInt64(max_args * max_arg_bytes), "arg.bufs")};
            auto* argc_i64{builder_.CreateSelect(builder_.CreateICmpSLT(raw_argc_i64, max_args_v),
                                                 raw_argc_i64,
                                                 max_args_v,
                                                 "argc.bounded")};

            auto* loop_cond{llvm::BasicBlock::Create(context_, "wargs.cond", entry_fn)};
            auto* loop_body{llvm::BasicBlock::Create(context_, "wargs.body", entry_fn)};
            auto* loop_inc{llvm::BasicBlock::Create(context_, "wargs.inc", entry_fn)};
            auto* loop_end{llvm::BasicBlock::Create(context_, "wargs.end", entry_fn)};

            auto* i_var{builder_.CreateAlloca(types_.get_int64_ty(), nullptr, "wi")};
            builder_.CreateStore(builder_.getInt64(0), i_var);
            builder_.CreateBr(loop_cond);

            builder_.SetInsertPoint(loop_cond);
            auto* cur_i{builder_.CreateLoad(types_.get_int64_ty(), i_var, "wcur.i")};
            auto* cmp{builder_.CreateICmpSLT(cur_i, argc_i64, "wcmp")};
            builder_.CreateCondBr(cmp, loop_body, loop_end);

            builder_.SetInsertPoint(loop_body);
            auto* wargv_elem_ptr{
                builder_.CreateGEP(types_.get_ptr_ty(), wargv, {cur_i}, "wargv.elem.ptr")};
            auto* wstr_ptr{builder_.CreateLoad(types_.get_ptr_ty(), wargv_elem_ptr, "wstr.ptr")};
            auto* buf_off{builder_.CreateMul(cur_i, builder_.getInt64(max_arg_bytes), "buf.off")};
            auto* buf_ptr{builder_.CreateGEP(types_.get_int8_ty(), arg_bufs, {buf_off}, "buf.ptr")};

            // Subtract one to exclude the converted null terminator, matching strlen-style.
            constexpr u32 auto_len{0xFFFFFFFFu}; // cchWideChar = -1: source is null-terminated
            auto*         converted{
                builder_.CreateCall(wc2mb_fn,
                                            {builder_.getInt32(cp_utf8),
                                             builder_.getInt32(0),
                                             wstr_ptr,
                                             builder_.getInt32(auto_len),
                                             buf_ptr,
                                             builder_.getInt32(max_arg_bytes),
                                             llvm::ConstantPointerNull::get(types_.get_ptr_ty()),
                                             llvm::ConstantPointerNull::get(types_.get_ptr_ty())},
                                    "converted.len")};
            auto* converted_i64{
                builder_.CreateSExt(converted, types_.get_int64_ty(), "converted.i64")};
            auto* final_len{builder_.CreateSub(converted_i64, builder_.getInt64(1), "final.len")};

            auto* dest_slice_ptr{
                builder_.CreateGEP(slice_ty, slice_array, {cur_i}, "wdest.slice.ptr")};
            auto* dest_data_field{
                builder_.CreateStructGEP(slice_ty,
                                         dest_slice_ptr,
                                         static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX),
                                         "wdest.data")};
            auto* dest_len_field{
                builder_.CreateStructGEP(slice_ty,
                                         dest_slice_ptr,
                                         static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX),
                                         "wdest.len")};
            builder_.CreateStore(buf_ptr, dest_data_field);
            builder_.CreateStore(final_len, dest_len_field);
            builder_.CreateBr(loop_inc);

            builder_.SetInsertPoint(loop_inc);
            auto* inc_i{builder_.CreateAdd(cur_i, builder_.getInt64(1), "winc.i")};
            builder_.CreateStore(inc_i, i_var);
            builder_.CreateBr(loop_cond);

            builder_.SetInsertPoint(loop_end);
            auto* outer_slice{builder_.CreateAlloca(slice_ty, nullptr, "outer.slice")};
            auto* outer_data{builder_.CreateStructGEP(
                slice_ty, outer_slice, static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX), "outer.data")};
            auto* outer_len{builder_.CreateStructGEP(
                slice_ty, outer_slice, static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX), "outer.len")};
            builder_.CreateStore(slice_array, outer_data);
            builder_.CreateStore(argc_i64, outer_len);
            outer_val = builder_.CreateLoad(slice_ty, outer_slice, "outer.val");
        }

        return outer_val;
    }

    auto* argc{entry_fn->getArg(0)};
    argc->setName("argc");
    auto* argv{entry_fn->getArg(1)};
    argv->setName("argv");

    auto* raw_argc_i64{builder_.CreateSExt(argc, types_.get_int64_ty(), "argc.i64")};

    // Fixed 128-slot buffer of {ptr, len} pairs; small enough to avoid stack probing.
    auto* max_args{builder_.getInt64(128)};
    auto* slice_array{builder_.CreateAlloca(slice_ty, max_args, "args.array")};
    auto* argc_i64{builder_.CreateSelect(
        builder_.CreateICmpSLT(raw_argc_i64, max_args), raw_argc_i64, max_args, "argc.bounded")};

    auto* loop_cond{llvm::BasicBlock::Create(context_, "loop.cond", entry_fn)};
    auto* loop_body{llvm::BasicBlock::Create(context_, "loop.body", entry_fn)};
    auto* loop_inc{llvm::BasicBlock::Create(context_, "loop.inc", entry_fn)};
    auto* loop_end{llvm::BasicBlock::Create(context_, "loop.end", entry_fn)};

    auto* i_var{builder_.CreateAlloca(types_.get_int64_ty(), nullptr, "i")};
    builder_.CreateStore(builder_.getInt64(0), i_var);
    builder_.CreateBr(loop_cond);

    // Loop condition: i < argc_i64
    builder_.SetInsertPoint(loop_cond);
    auto* cur_i{builder_.CreateLoad(types_.get_int64_ty(), i_var, "cur.i")};
    auto* cmp{builder_.CreateICmpSLT(cur_i, argc_i64, "cmp")};
    builder_.CreateCondBr(cmp, loop_body, loop_end);

    // Loop body: measure strlen(argv[i]) and store into slice_array[i]
    builder_.SetInsertPoint(loop_body);
    auto* argv_elem_ptr{builder_.CreateGEP(types_.get_ptr_ty(), argv, {cur_i}, "argv.elem.ptr")};
    auto* str_ptr{builder_.CreateLoad(types_.get_ptr_ty(), argv_elem_ptr, "str.ptr")};

    // Calculate strlen(str_ptr) via a sub-loop
    auto* str_len_cond{llvm::BasicBlock::Create(context_, "strlen.cond", entry_fn)};
    auto* str_len_body{llvm::BasicBlock::Create(context_, "strlen.body", entry_fn)};
    auto* str_len_end{llvm::BasicBlock::Create(context_, "strlen.end", entry_fn)};

    auto* len_var{builder_.CreateAlloca(types_.get_int64_ty(), nullptr, "str.len")};
    builder_.CreateStore(builder_.getInt64(0), len_var);
    builder_.CreateBr(str_len_cond);

    builder_.SetInsertPoint(str_len_cond);
    auto* cur_len{builder_.CreateLoad(types_.get_int64_ty(), len_var, "cur.len")};
    auto* char_ptr{builder_.CreateGEP(types_.get_int8_ty(), str_ptr, {cur_len}, "char.ptr")};
    auto* char_val{builder_.CreateLoad(types_.get_int8_ty(), char_ptr, "char.val")};
    auto* is_not_null{builder_.CreateICmpNE(char_val, builder_.getInt8(0), "not.null")};
    builder_.CreateCondBr(is_not_null, str_len_body, str_len_end);

    builder_.SetInsertPoint(str_len_body);
    auto* next_len{builder_.CreateAdd(cur_len, builder_.getInt64(1), "next.len")};
    builder_.CreateStore(next_len, len_var);
    builder_.CreateBr(str_len_cond);

    builder_.SetInsertPoint(str_len_end);
    auto* final_len{builder_.CreateLoad(types_.get_int64_ty(), len_var, "final.len")};

    // Store { str_ptr, final_len } into slice_array[cur_i]
    auto* dest_slice_ptr{builder_.CreateGEP(slice_ty, slice_array, {cur_i}, "dest.slice.ptr")};
    auto* dest_data_field{builder_.CreateStructGEP(
        slice_ty, dest_slice_ptr, static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX), "dest.data")};
    auto* dest_len_field{builder_.CreateStructGEP(
        slice_ty, dest_slice_ptr, static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX), "dest.len")};
    builder_.CreateStore(str_ptr, dest_data_field);
    builder_.CreateStore(final_len, dest_len_field);
    builder_.CreateBr(loop_inc);

    // Loop increment: i++
    builder_.SetInsertPoint(loop_inc);
    auto* inc_i{builder_.CreateAdd(cur_i, builder_.getInt64(1), "inc.i")};
    builder_.CreateStore(inc_i, i_var);
    builder_.CreateBr(loop_cond);

    builder_.SetInsertPoint(loop_end);
    auto* outer_slice{builder_.CreateAlloca(slice_ty, nullptr, "outer.slice")};
    auto* outer_data{builder_.CreateStructGEP(
        slice_ty, outer_slice, static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX), "outer.data")};
    auto* outer_len{builder_.CreateStructGEP(
        slice_ty, outer_slice, static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX), "outer.len")};
    builder_.CreateStore(slice_array, outer_data);
    builder_.CreateStore(argc_i64, outer_len);
    return builder_.CreateLoad(slice_ty, outer_slice, "outer.val");
}

auto llvm_lowering::lower_test_executable(const gir::module&             gir_mod,
                                          stdx::option<std::string_view> user_runner_name,
                                          bool recover_args) -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    user_main_name_ =
        user_runner_name.transform([](auto sv) { return std::string{sv}; }).value_or(std::string{});
    is_executable_ = true;
    gir_module_.emplace(gir_mod);
    for (const auto* fn : gir_mod.get_functions()) { declare_function(*fn); }
    define_test_take_skipped();
    for (const auto* global : gir_mod.get_globals()) { lower_global(*global); }
    lower_dyn_vtables(gir_mod);
    for (const auto* fn : gir_mod.get_functions()) { lower_function(*fn); }
    emit_test_entry_wrapper(gir_mod, recover_args);
    maybe_emit_windows_stack_probe();
    return std::move(llvm_module_);
}

auto llvm_lowering::emit_test_entry_wrapper(const gir::module& gir_mod, bool recover_args)
    -> llvm::Function* {
    PROFILE_FUNCTION();
    auto* main_fn_ty{llvm::FunctionType::get(
        types_.get_int32_ty(), {types_.get_int32_ty(), types_.get_ptr_ty()}, false)};
    auto* main_fn{llvm::Function::Create(
        main_fn_ty, llvm::Function::ExternalLinkage, "main", llvm_module_.get())};
    main_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    main_fn->addFnAttr("no-builtins");
    main_fn->addFnAttr("no-stack-arg-probe", "true");

    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (triple.isWindowsGNUEnvironment() && !llvm_module_->getFunction("__main")) {
        auto*             void_ty{llvm::Type::getVoidTy(context_)};
        auto*             dummy_main_ty{llvm::FunctionType::get(void_ty, false)};
        auto*             dummy_main{llvm::Function::Create(
            dummy_main_ty, llvm::Function::ExternalLinkage, "__main", llvm_module_.get())};
        auto*             dummy_bb{llvm::BasicBlock::Create(context_, "entry", dummy_main)};
        llvm::IRBuilder<> dummy_builder{dummy_bb};
        dummy_builder.CreateRetVoid();
        llvm::appendToUsed(*llvm_module_, {dummy_main});
    }

    auto* slice_ty{types_.translate_slice_type()};
    // Mirrors `builtin::Test { name: []u8, file: []u8, line: u32, column: u32, func: fn(): bool }`
    auto* test_struct_ty{llvm::StructType::get(
        context_,
        {slice_ty, slice_ty, types_.get_int32_ty(), types_.get_int32_ty(), types_.get_ptr_ty()})};

    const auto& test_fns{gir_mod.get_test_functions()};
    const auto  test_count{test_fns.size()};

    std::vector<llvm::Constant*> test_descriptors;
    test_descriptors.reserve(test_count);

    for (usize i{0}; i < test_count; ++i) {
        const auto* fn{test_fns[i]};
        const auto& fn_symbol_name{fn->get_name()};
        auto*       test_llvm_fn{llvm_module_->getFunction(fn_symbol_name)};
        ASSERT(test_llvm_fn, "Test function must already be declared before the entry wrapper");

        const auto& desc_name{fn->get_test_desc().empty() ? fn_symbol_name : fn->get_test_desc()};
        auto*       name_str{llvm::ConstantDataArray::getString(context_, desc_name, false)};
        auto*       name_gvar{new llvm::GlobalVariable(*llvm_module_,
                                                 name_str->getType(),
                                                 true,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 name_str,
                                                 fmt::format("str.test_name.{}", i))};
        auto*       name_slice{llvm::ConstantStruct::get(
            slice_ty,
            {name_gvar, llvm::ConstantInt::get(types_.get_usize_ty(), desc_name.size())})};

        const auto file_path{fn->get_test_file().empty() ? gir_mod.get_ast_module().path.string()
                                                         : fn->get_test_file()};
        auto*      file_str{llvm::ConstantDataArray::getString(context_, file_path, false)};
        auto*      file_gvar{new llvm::GlobalVariable(*llvm_module_,
                                                 file_str->getType(),
                                                 true,
                                                 llvm::GlobalValue::InternalLinkage,
                                                 file_str,
                                                 fmt::format("str.test_file.{}", i))};
        auto*      file_slice{llvm::ConstantStruct::get(
            slice_ty,
            {file_gvar, llvm::ConstantInt::get(types_.get_usize_ty(), file_path.size())})};

        auto* line_val{llvm::ConstantInt::get(types_.get_int32_ty(), fn->get_test_line())};
        auto* col_val{llvm::ConstantInt::get(types_.get_int32_ty(), fn->get_test_column())};

        test_descriptors.emplace_back(llvm::ConstantStruct::get(
            test_struct_ty, {name_slice, file_slice, line_val, col_val, test_llvm_fn}));
    }

    auto* test_array_ty{llvm::ArrayType::get(test_struct_ty, std::max(test_count, usize{1}))};
    auto* test_array_init{test_count > 0 ? llvm::ConstantArray::get(test_array_ty, test_descriptors)
                                         : llvm::ConstantAggregateZero::get(test_array_ty)};
    auto* test_array_gvar{new llvm::GlobalVariable(*llvm_module_,
                                                   test_array_ty,
                                                   true,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   test_array_init,
                                                   "__ghoti_tests_data")};

    auto* test_slice_const{llvm::ConstantStruct::get(
        slice_ty, {test_array_gvar, llvm::ConstantInt::get(types_.get_usize_ty(), test_count)})};
    auto* test_slice_gvar{new llvm::GlobalVariable(*llvm_module_,
                                                   slice_ty,
                                                   true,
                                                   llvm::GlobalValue::InternalLinkage,
                                                   test_slice_const,
                                                   "__ghoti_tests_slice")};

    auto* entry_bb{llvm::BasicBlock::Create(context_, "entry", main_fn)};
    builder_.SetInsertPoint(entry_bb);

    // The runner is always `test_runner(args: [][:0]u8, tests: []Test): i32`
    auto* runner_fn{llvm_module_->getFunction("test_runner")};
    VERIFY(runner_fn, "a `test_runner` (builtin weak default or user override) must be linked");

    auto* args_slice_val{emit_argv_slice(main_fn, recover_args)};
    auto* test_slice_val{builder_.CreateLoad(slice_ty, test_slice_gvar, "tests.slice")};
    auto* call_res{builder_.CreateCall(runner_fn, {args_slice_val, test_slice_val})};
    if (runner_fn->getReturnType()->isIntegerTy(32)) {
        builder_.CreateRet(call_res);
    } else {
        builder_.CreateRet(builder_.getInt32(0));
    }

    // Same freestanding-entry story as a normal executable (see emit_main_entry_wrapper).
    if (triple.isOSLinux()) { emit_freestanding_start(main_fn); }

    return main_fn;
}

auto llvm_lowering::get_or_create_test_failed_flag() -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (auto* gvar{llvm_module_->getGlobalVariable("__ghoti_test_failed", true)}) { return gvar; }
    return new llvm::GlobalVariable(*llvm_module_,
                                    builder_.getInt1Ty(),
                                    false,
                                    llvm::GlobalValue::InternalLinkage,
                                    builder_.getInt1(false),
                                    "__ghoti_test_failed");
}

auto llvm_lowering::get_or_create_test_skipped_flag() -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (auto* gvar{llvm_module_->getGlobalVariable("__ghoti_test_skipped", true)}) { return gvar; }
    return new llvm::GlobalVariable(*llvm_module_,
                                    builder_.getInt1Ty(),
                                    false,
                                    llvm::GlobalValue::InternalLinkage,
                                    builder_.getInt1(false),
                                    "__ghoti_test_skipped");
}

auto llvm_lowering::define_test_take_skipped() -> void {
    PROFILE_FUNCTION();
    auto* i1_ty{builder_.getInt1Ty()};
    auto* fn{llvm_module_->getFunction("__ghoti_test_take_skipped")};
    if (!fn) {
        auto* fn_ty{llvm::FunctionType::get(i1_ty, {}, false)};
        fn = llvm::Function::Create(fn_ty,
                                    llvm::Function::InternalLinkage,
                                    "__ghoti_test_take_skipped",
                                    llvm_module_.get());
    } else {
        fn->setLinkage(llvm::Function::InternalLinkage);
    }
    if (!fn->empty()) { return; }

    auto*             bb{llvm::BasicBlock::Create(context_, "entry", fn)};
    llvm::IRBuilder<> b{bb};
    auto*             flag{get_or_create_test_skipped_flag()};
    auto*             was_skipped{b.CreateLoad(i1_ty, flag, "skipped")};
    b.CreateStore(b.getInt1(false), flag);
    b.CreateRet(was_skipped);
}

auto llvm_lowering::emit_context_handler_call(const gir::instruction&   inst,
                                              std::string_view          handler_name,
                                              gsl::span<const usize, 4> order) -> void {
    PROFILE_FUNCTION();
    auto* handler_fn{llvm_module_->getFunction(handler_name)};
    if (!handler_fn) { return; }

    auto* fn_ty{handler_fn->getFunctionType()};
    if (fn_ty->getNumParams() != 4) { return; }
    for (const auto idx : order) {
        if (idx >= inst.operands.size()) { return; }
    }

    std::vector<llvm::Value*> args;
    args.reserve(4);
    for (u32 param_idx{0}; param_idx < 4; ++param_idx) {
        auto*       param_ty{fn_ty->getParamType(param_idx)};
        const auto& op{inst.operands[order[param_idx]]};

        if (const auto str{op.as_opt<std::string>()}) {
            // A raw string operand (`file`, `msg`) becomes a `[]u8` slice `{ ptr, len }`.
            auto* gstr{builder_.CreateGlobalString(*str, "ch.str")};
            if (param_ty->isStructTy()) {
                llvm::Value* slice{llvm::UndefValue::get(param_ty)};
                slice = builder_.CreateInsertValue(
                    slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                slice = builder_.CreateInsertValue(slice,
                                                   builder_.getInt64(str->size()),
                                                   {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
                args.push_back(slice);
            } else {
                args.push_back(gstr);
            }
            continue;
        }

        auto* v{lower_value(op)};
        if (!v) { return; }
        if (v->getType() != param_ty && v->getType()->isIntegerTy() && param_ty->isIntegerTy()) {
            v = builder_.CreateZExtOrTrunc(v, param_ty);
        }
        args.push_back(v);
    }

    builder_.CreateCall(handler_fn, args);
}

auto llvm_lowering::emit_lowered_panic(std::string_view message, const gir::instruction& inst)
    -> void {
    PROFILE_FUNCTION();
    auto* panic_fn{llvm_module_->getFunction("panic_handler")};
    if (!panic_fn || panic_fn->getFunctionType()->getNumParams() != 4) {
        auto* trap{
            llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
        builder_.CreateCall(trap, {});
        return;
    }

    auto*      slice_ty{types_.translate_slice_type()};
    const auto make_slice{[&](std::string_view text) -> llvm::Value* {
        auto*        gstr{builder_.CreateGlobalString(std::string{text}, "panic.str")};
        llvm::Value* slice{llvm::UndefValue::get(slice_ty)};
        slice =
            builder_.CreateInsertValue(slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
        slice = builder_.CreateInsertValue(
            slice, builder_.getInt64(text.size()), {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
        return slice;
    }};

    const auto                loc{inst.location.value_or(source_location{})};
    std::vector<llvm::Value*> args{
        make_slice(message),
        make_slice(llvm_module_->getName()),
        builder_.getInt32(static_cast<u32>(loc.line)),
        builder_.getInt32(static_cast<u32>(loc.column)),
    };
    builder_.CreateCall(panic_fn, args);
}

auto llvm_lowering::emit_arith_guard(llvm::Value*            bad,
                                     std::string_view        message,
                                     const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    auto* cur_fn{builder_.GetInsertBlock()->getParent()};
    auto* fail_bb{llvm::BasicBlock::Create(context_, "safety.fail", cur_fn)};
    auto* ok_bb{llvm::BasicBlock::Create(context_, "safety.ok", cur_fn)};
    builder_.CreateCondBr(bad, fail_bb, ok_bb);

    builder_.SetInsertPoint(fail_bb);
    emit_lowered_panic(message, inst);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(ok_bb);
}

auto llvm_lowering::emit_checked_arith(const gir::instruction& inst,
                                       llvm::Value*            lhs,
                                       llvm::Value*            rhs,
                                       bool                    is_signed) -> llvm::Value* {
    PROFILE_FUNCTION();
    auto* int_ty{llvm::dyn_cast<llvm::IntegerType>(lhs->getType())};
    if (!int_ty) { return nullptr; }
    const unsigned width{int_ty->getBitWidth()};

    switch (inst.kind) {
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL: {
        if (!is_signed) { return nullptr; } // unsigned wrap is defined
        const auto id{inst.kind == gir::instruction_kind::ADD ? llvm::Intrinsic::sadd_with_overflow
                      : inst.kind == gir::instruction_kind::SUB
                          ? llvm::Intrinsic::ssub_with_overflow
                          : llvm::Intrinsic::smul_with_overflow};
        auto*      fn{llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, {int_ty})};
        auto*      pair{builder_.CreateCall(fn, {lhs, rhs})};
        auto*      overflow{builder_.CreateExtractValue(pair, {1U})};

        std::string_view msg;
        if (inst.kind == gir::instruction_kind::ADD) {
            msg = "signed addition overflow";
        } else if (inst.kind == gir::instruction_kind::SUB) {
            msg = "signed subtraction overflow";
        } else {
            msg = "signed multiplication overflow";
        }
        emit_arith_guard(overflow, msg, inst);
        return builder_.CreateExtractValue(pair, {0U});
    }
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD: {
        auto* zero{llvm::ConstantInt::get(int_ty, 0)};
        emit_arith_guard(builder_.CreateICmpEQ(rhs, zero), "integer division by zero", inst);
        if (is_signed) {
            auto* int_min{llvm::ConstantInt::get(int_ty, llvm::APInt::getSignedMinValue(width))};
            auto* neg_one{llvm::ConstantInt::getSigned(int_ty, -1)};
            auto* edge{builder_.CreateAnd(builder_.CreateICmpEQ(lhs, int_min),
                                          builder_.CreateICmpEQ(rhs, neg_one))};
            emit_arith_guard(edge, "signed division overflow", inst);
        }
        return nullptr;
    }
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR: {
        auto* limit{llvm::ConstantInt::get(rhs->getType(), width)};
        emit_arith_guard(builder_.CreateICmpUGE(rhs, limit), "shift amount out of range", inst);
        return nullptr;
    }
    default: return nullptr;
    }
}

auto llvm_lowering::const_to_llvm(const gir::const_value& cv, llvm::Type* ty) -> llvm::Constant* {
    PROFILE_FUNCTION();
    if (!ty) { return nullptr; }
    if (const auto i{cv.as_opt<i64>()}) { return int_or_ptr_constant(static_cast<u128>(*i), ty); }
    if (const auto u{cv.as_opt<u64>()}) { return int_or_ptr_constant(*u, ty); }
    if (const auto w{cv.as_opt<i128>()}) { return int_or_ptr_constant(static_cast<u128>(*w), ty); }
    if (const auto w{cv.as_opt<u128>()}) { return int_or_ptr_constant(*w, ty); }
    if (const auto f{cv.as_opt<f64>()}) { return llvm::ConstantFP::get(ty, *f); }
    if (const auto b{cv.as_opt<bool>()}) { return llvm::ConstantInt::getBool(context_, *b); }
    if (const auto e{cv.as_opt<gir::const_enum>()}) {
        return llvm::ConstantInt::get(ty, static_cast<u64>(e->value), true);
    }
    if (const auto s{cv.as_opt<std::string>()}) {
        if (ty->isPointerTy()) {
            if (auto* fn{resolve_named_function(*s)}) { return fn; }
            return builder_.CreateGlobalString(*s, "gstr");
        }
        if (auto* at{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            std::string bytes{*s};
            bytes.resize(static_cast<usize>(at->getNumElements()), '\0');
            return llvm::ConstantDataArray::getString(context_, bytes, false);
        }
        return llvm::Constant::getNullValue(ty);
    }
    if (const auto st{cv.as_opt<gir::const_struct>()}) {
        auto*      llvm_st{llvm::dyn_cast<llvm::StructType>(ty)};
        const auto sema_ty{cv.get_type()};
        const auto sd{sema_ty ? sema_ty->get_data().as_opt<sema::types::struct_t>() : stdx::none};
        if (!llvm_st || !sd) { return llvm::Constant::getNullValue(ty); }
        std::vector<llvm::Constant*> elems;
        elems.reserve(sd->fields.size());
        for (usize i{0}; i < sd->fields.size() && i < llvm_st->getNumElements(); ++i) {
            const auto& fname{
                sd->enclosing.ast.get_as<ast::identifier_expr>(sd->ast_fields[i].name).name};
            auto* felem_ty{llvm_st->getElementType(static_cast<u32>(i))};
            if (const auto fv{st->get_field_opt(fname)}) {
                elems.emplace_back(const_to_llvm(*fv, felem_ty));
            } else {
                elems.emplace_back(llvm::Constant::getNullValue(felem_ty));
            }
        }
        return llvm::ConstantStruct::get(llvm_st, elems);
    }
    if (const auto arr{cv.as_opt<gir::const_array>()}) {
        auto* at{llvm::dyn_cast<llvm::ArrayType>(ty)};
        if (!at) { return llvm::Constant::getNullValue(ty); }
        auto*                        elem_ty{at->getElementType()};
        std::vector<llvm::Constant*> elems;
        elems.reserve(static_cast<usize>(at->getNumElements()));
        for (const auto& elem : arr->elements) { elems.emplace_back(const_to_llvm(elem, elem_ty)); }
        while (elems.size() < at->getNumElements()) {
            elems.emplace_back(llvm::Constant::getNullValue(elem_ty));
        }
        return llvm::ConstantArray::get(at, elems);
    }
    return llvm::Constant::getNullValue(ty);
}

auto llvm_lowering::lower_global(const gir::global_decl& g) -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(g.name)}; it != globals_.end()) {
        if (auto* existing{llvm::dyn_cast<llvm::GlobalVariable>(it->second)}) { return existing; }
    }
    if (auto* existing{llvm_module_->getGlobalVariable(g.name)}) { return existing; }
    ensure_reserved_symbols();

    auto*      g_type{types_.translate(g.type)};
    const bool is_const{g.is_constant};
    auto       g_linkage{(g.linkage == gir::linkage::INTERNAL) ? llvm::GlobalValue::InternalLinkage
                                                               : llvm::GlobalValue::ExternalLinkage};
    if (g.is_weak && g_linkage != llvm::GlobalValue::InternalLinkage) {
        g_linkage = (g.linkage == gir::linkage::EXTERN) ? llvm::GlobalValue::ExternalWeakLinkage
                                                        : llvm::GlobalValue::WeakAnyLinkage;
    }

    llvm::Constant* init{nullptr};
    if (g.const_init) {
        init = const_to_llvm(*g.const_init, g_type);
    } else if (g.init_value) {
        auto* init_v{lower_value(*g.init_value, &g.type)};
        if (init_v && llvm::isa<llvm::Constant>(init_v)) {
            init = llvm::cast<llvm::Constant>(init_v);
        }
    }

    if (init == nullptr && g.linkage != gir::linkage::EXTERN) {
        init = llvm::Constant::getNullValue(g_type);
    }

    std::string sym_name{g.link_name.empty() ? g.name : g.link_name};
    // A plain (INTERNAL/PUBLIC) ghoti global whose name collides with a C-ABI symbol
    if (g.link_name.empty() && !g.is_weak &&
        (llvm_module_->getNamedValue(sym_name) != nullptr ||
         reserved_symbols_.contains(sym_name))) {
        sym_name  = private_symbol_name(sym_name);
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }
    auto* gvar{
        new llvm::GlobalVariable{*llvm_module_, g_type, is_const, g_linkage, init, sym_name}};
    if (g.is_thread_local) {
        gvar->setThreadLocalMode(is_executable_ ? llvm::GlobalValue::LocalExecTLSModel
                                                : llvm::GlobalValue::GeneralDynamicTLSModel);
    }
    globals_[g.name] = gvar;
    return gvar;
}

auto llvm_lowering::ensure_reserved_symbols() -> void {
    PROFILE_FUNCTION();
    if (reserved_symbols_built_) { return; }
    reserved_symbols_built_ = true;
    if (!gir_module_) { return; }
    for (const auto* f : gir_module_->get_functions()) {
        if (!f->get_link_name().empty()) { reserved_symbols_.emplace(f->get_link_name()); }
    }
    for (const auto* g : gir_module_->get_globals()) {
        if (!g->link_name.empty()) { reserved_symbols_.emplace(g->link_name); }
    }
}

auto llvm_lowering::private_symbol_name(std::string_view name) const -> std::string {
    auto candidate{fmt::format("__ghoti.{}", name)};
    for (u32 n{1}; llvm_module_->getNamedValue(candidate) != nullptr; ++n) {
        candidate = fmt::format("__ghoti.{}.{}", name, n);
    }
    return candidate;
}

auto llvm_lowering::declare_function(const gir::function& fn) -> llvm::Function* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(fn.get_name())}; it != globals_.end()) {
        if (auto* existing{llvm::dyn_cast<llvm::Function>(it->second)}) { return existing; }
    }
    ensure_reserved_symbols();

    const auto* target_t{&fn.get_type()};
    if (const auto ref{target_t->get_data().as_opt<sema::types::reference>()}) {
        target_t = &ref->underlying;
    }
    if (const auto ptr{target_t->get_data().as_opt<sema::types::pointer>()}) {
        target_t = &ptr->underlying;
    }

    const auto fn_data{target_t->get_data().as_opt<sema::types::function>()};
    ASSERT(fn_data, "Function must have function sema type");
    auto* fn_ty{types_.translate_function_type(*fn_data)};

    std::string fn_name{fn.get_name()};
    auto        g_linkage{llvm::GlobalValue::ExternalLinkage};
    if (fn.get_linkage() == gir::linkage::INTERNAL) {
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    // Whether this function's symbol name is externally meaningful and must be kept verbatim
    bool fixed_symbol{false};
    if (is_executable_ && fn_name == user_main_name_) {
        fn_name      = "_ghoti_main";
        g_linkage    = llvm::GlobalValue::InternalLinkage;
        fixed_symbol = true;
    } else if (!fn.get_link_name().empty()) {
        fn_name      = std::string{fn.get_link_name()};
        fixed_symbol = true;
    }

    if (fn.get_is_weak() && g_linkage != llvm::GlobalValue::InternalLinkage) {
        g_linkage    = (fn.get_linkage() == gir::linkage::EXTERN)
                           ? llvm::GlobalValue::ExternalWeakLinkage
                           : llvm::GlobalValue::WeakAnyLinkage;
        fixed_symbol = true;
    }

    auto* existing{llvm_module_->getFunction(fn_name)};
    if (existing && (fixed_symbol || existing->getFunctionType() == fn_ty)) { return existing; }

    // An internal/pub ghoti definition whose natural name collides with a C-ABI symbol
    if (!fixed_symbol && (existing != nullptr || reserved_symbols_.contains(fn_name))) {
        fn_name   = private_symbol_name(fn_name);
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    auto* llvm_fn{llvm::Function::Create(fn_ty, g_linkage, fn_name, llvm_module_.get())};
    llvm_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    // Non x86-64 Windows links no runtime that could provide a probe symbol
    if (!windows_stack_probe_symbol()) { llvm_fn->addFnAttr("no-stack-arg-probe", "true"); }
    if (fn.get_calling_conv() != ast::calling_convention::C) {
        llvm_fn->setCallingConv(to_llvm_callconv(fn.get_calling_conv()));
    }
    if (fn.get_is_naked()) {
        llvm_fn->addFnAttr(llvm::Attribute::Naked);
        llvm_fn->addFnAttr(llvm::Attribute::NoInline);
    }
    for (usize arg_idx{0}; const auto& param : fn.get_params()) {
        auto* p_ty{types_.translate(param->type)};
        if (p_ty->isVoidTy()) { continue; }
        auto* arg{llvm_fn->getArg(static_cast<u32>(arg_idx++))};
        arg->setName(param->name);
    }

    globals_[fn.get_name()] = llvm_fn;
    return llvm_fn;
}

auto llvm_lowering::lower_function(const gir::function& fn) -> llvm::Function* {
    PROFILE_FUNCTION();
    auto* llvm_fn{declare_function(fn)};
    if (fn.get_linkage() == gir::linkage::EXTERN || fn.get_segments().empty()) { return llvm_fn; }
    clear_locals();

    // Pre-allocate basic blocks for all segments
    for (const auto* seg : fn.get_segments()) {
        auto* bb{llvm::BasicBlock::Create(
            context_, fmt::format("seg_{}", std::to_underlying(seg->get_id())), llvm_fn)};
        segment_blocks_[seg->get_id()] = bb;
    }

    // Bind parameters; spilled slots (if any) go at the top of the entry block
    builder_.SetInsertPoint(segment_blocks_[fn.get_segments()[0]->get_id()]);
    for (usize arg_idx{0}; const auto& param : fn.get_params()) {
        auto* p_ty{types_.translate(param->type)};
        if (p_ty->isVoidTy()) { continue; }
        auto* arg{llvm_fn->getArg(static_cast<u32>(arg_idx++))};
        if (param->type.get_kind() == sema::type_kind::SLICE) {
            // Slices need to be addressable for runtime-indexed element access so spill the
            // incoming by-value slice to the stack.
            auto* slot{builder_.CreateAlloca(p_ty, nullptr, "param_slot")};
            builder_.CreateStore(arg, slot);
            set_local(param->id, slot);
        } else {
            set_local(param->id, arg);
        }
    }

    if (fn.get_is_test()) {
        builder_.CreateStore(builder_.getInt1(false), get_or_create_test_failed_flag());
        builder_.CreateStore(builder_.getInt1(false), get_or_create_test_skipped_flag());
    }

    // Lower each segment
    for (const auto* seg : fn.get_segments()) {
        auto* bb{segment_blocks_[seg->get_id()]};
        builder_.SetInsertPoint(bb);

        for (const auto* inst : seg->get_instructions()) { lower_instruction(*inst); }
        if (!bb->getTerminator()) {
            const sema::type* target_ret{&fn.get_type()};
            if (const auto ref{target_ret->get_data().as_opt<sema::types::reference>()}) {
                target_ret = &ref->underlying;
            }
            if (const auto ptr{target_ret->get_data().as_opt<sema::types::pointer>()}) {
                target_ret = &ptr->underlying;
            }
            const auto fn_ret_data{target_ret->get_data().as_opt<sema::types::function>()};
            if (fn_ret_data && fn_ret_data->return_type.get_kind() == sema::type_kind::VOID_) {
                builder_.CreateRetVoid();
            } else {
                builder_.CreateUnreachable();
            }
        }
    }

    if (fn.get_is_test()) {
        auto*                          flag{get_or_create_test_failed_flag()};
        std::vector<llvm::ReturnInst*> rets;
        for (auto& bb : *llvm_fn) {
            if (auto* ret{llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())}) {
                if (auto* rv{ret->getReturnValue()}; rv && rv->getType()->isIntegerTy(1)) {
                    rets.push_back(ret);
                }
            }
        }
        for (auto* ret : rets) {
            llvm::IRBuilder<> rb{ret};
            auto*             failed{rb.CreateLoad(rb.getInt1Ty(), flag, "test.failed")};
            auto*             combined{
                rb.CreateAnd(ret->getReturnValue(), rb.CreateNot(failed), "test.result")};
            rb.CreateRet(combined);
            ret->eraseFromParent();
        }
    }

    return llvm_fn;
}

auto llvm_lowering::lower_value(const gir::value& val, const sema::type* expected_type)
    -> llvm::Value* {
    PROFILE_FUNCTION();
    return val.data.visit(
        [this](gir::local_id loc) -> llvm::Value* {
            const auto it{locals_.find(loc)};
            ASSERT(it != locals_.end(), "Referenced GIR local was never lowered");
            return it->second;
        },
        [this, &val, expected_type](i64 i) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(static_cast<u128>(i), ty);
        },
        [this, &val, expected_type](u64 u) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(u, ty);
        },
        [this, &val, expected_type](i128 w) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(static_cast<u128>(w), ty);
        },
        [this, &val, expected_type](u128 w) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(w, ty);
        },
        [this, &val, expected_type](f64 f) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_double_ty()};
            return llvm::ConstantFP::get(ty, f);
        },
        [this](bool b) -> llvm::Value* { return llvm::ConstantInt::getBool(context_, b); },
        [this, &val, expected_type](const std::string& str) -> llvm::Value* {
            const sema::type* ty{expected_type};
            if (!ty && val.type) { ty = &*val.type; }
            // A fn-typed string value names a function rather than holding string data
            if (ty && ty->get_kind() == sema::type_kind::FUNCTION) {
                if (auto* fn{resolve_named_function(str)}) { return fn; }
                // Not yet declared in this translation unit for the linnker to resolve
                const auto fn_data{ty->get_data().as_opt<sema::types::function>()};
                ASSERT(fn_data, "FUNCTION-typed value must carry function type data");
                auto* fn_ty{types_.translate_function_type(*fn_data)};
                return llvm::Function::Create(
                    fn_ty, llvm::Function::ExternalLinkage, str, llvm_module_.get());
            }
            if (ty && ty->get_kind() == sema::type_kind::ARRAY) {
                return llvm::ConstantDataArray::getString(context_, str, true);
            }
            return builder_.CreateGlobalString(str, "str");
        },
        [this, &val, expected_type](gir::undefined_val) -> llvm::Value* {
            auto* ty{val.type        ? types_.translate(*val.type)
                     : expected_type ? types_.translate(*expected_type)
                                     : types_.get_int64_ty()};
            if (ty->isVoidTy()) { return nullptr; }
            return llvm::UndefValue::get(ty);
        },
        [](gir::void_val) -> llvm::Value* { return nullptr; },
        [this](gir::nullptr_val) -> llvm::Value* {
            return llvm::ConstantPointerNull::get(types_.get_ptr_ty());
        },
        [](const stdx::option<sema::type&>&) -> llvm::Value* { return nullptr; });
}

auto llvm_lowering::lower_instruction(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    llvm::Value* result_val{nullptr};

    switch (inst.kind) {
    case gir::instruction_kind::ALLOCA:          result_val = emit_alloca(inst); break;
    case gir::instruction_kind::LOAD:            result_val = emit_load(inst); break;
    case gir::instruction_kind::STORE:           emit_store(inst); return;
    case gir::instruction_kind::GET_ELEMENT_PTR: result_val = emit_get_element_ptr(inst); break;
    case gir::instruction_kind::ADDRESS_OF:      result_val = emit_address_of(inst); break;
    case gir::instruction_kind::DEREF:           result_val = emit_deref(inst); break;
    case gir::instruction_kind::GLOBAL_ADDR:     result_val = emit_global_addr(inst); break;
    case gir::instruction_kind::CONSTANT:        result_val = emit_const(inst); break;
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL:
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD:
    case gir::instruction_kind::AND:
    case gir::instruction_kind::OR:
    case gir::instruction_kind::XOR:
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR:             result_val = emit_binary(inst); break;
    case gir::instruction_kind::NEG:
    case gir::instruction_kind::NOT:
    case gir::instruction_kind::BITNOT:          result_val = emit_unary(inst); break;
    case gir::instruction_kind::EQ:
    case gir::instruction_kind::NE:
    case gir::instruction_kind::LT:
    case gir::instruction_kind::LE:
    case gir::instruction_kind::GT:
    case gir::instruction_kind::GE:              result_val = emit_comparison(inst); break;
    case gir::instruction_kind::WIDEN_CAST:
    case gir::instruction_kind::BIT_CAST:
    case gir::instruction_kind::PTR_CAST:
    case gir::instruction_kind::INT_FROM_PTR:
    case gir::instruction_kind::PTR_FROM_INT:    result_val = emit_cast(inst); break;
    case gir::instruction_kind::CALL:            result_val = emit_call(inst); break;
    case gir::instruction_kind::BUILTIN_CALL:    result_val = emit_builtin_call(inst); break;
    case gir::instruction_kind::INLINE_ASM:      result_val = emit_inline_asm(inst); break;
    case gir::instruction_kind::RET:             emit_ret(inst); return;
    case gir::instruction_kind::GOTO:            emit_goto(inst); return;
    case gir::instruction_kind::COND_GOTO:       emit_cond_goto(inst); return;
    case gir::instruction_kind::UNREACHABLE:     emit_unreachable(); return;
    }

    if (result_val && inst.result) { set_local(*inst.result, result_val); }
}

auto llvm_lowering::emit_alloca(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.type, "Alloca requires a type");
    auto* elem_ty{types_.translate(*inst.type)};
    if (elem_ty->isVoidTy()) {
        auto* dummy{llvm::UndefValue::get(types_.get_ptr_ty())};
        if (inst.result) { set_local(*inst.result, dummy); }
        return dummy;
    }

    auto* slot{builder_.CreateAlloca(elem_ty, nullptr, "slot")};
    if (inst.result) { set_local(*inst.result, slot); }
    return slot;
}

auto llvm_lowering::emit_load(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Load requires a pointer operand");
    ASSERT(inst.type, "Load requires a target type");
    auto* elem_ty{types_.translate(*inst.type)};
    if (elem_ty->isVoidTy()) {
        if (inst.result) { set_local(*inst.result, nullptr); }
        return nullptr;
    }

    auto* ptr{lower_value(inst.operands[0])};
    if (!ptr) {
        if (inst.result) { set_local(*inst.result, nullptr); }
        return nullptr;
    }

    if (!ptr->getType()->isPointerTy()) {
        if (inst.result) { set_local(*inst.result, ptr); }
        return ptr;
    }

    auto* loaded{builder_.CreateLoad(elem_ty, ptr, inst.is_volatile(), "loadtmp")};
    if (inst.result) { set_local(*inst.result, loaded); }
    return loaded;
}

auto llvm_lowering::emit_store(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    const auto is_volatile{inst.is_volatile()};
    if (inst.operands.size() >= 2) {
        auto* dest_ptr{lower_value(inst.operands[0])};
        auto* val{lower_value(inst.operands[1], inst.type ? &*inst.type : nullptr)};
        if (!dest_ptr || !dest_ptr->getType()->isPointerTy() || !val ||
            val->getType()->isVoidTy()) {
            return;
        }
        builder_.CreateStore(val, dest_ptr, is_volatile);
    } else if (inst.result && !inst.operands.empty()) {
        auto* dest_ptr{lower_value(gir::value{*inst.result})};
        auto* val{lower_value(inst.operands[0], inst.type ? &*inst.type : nullptr)};
        if (!dest_ptr || !dest_ptr->getType()->isPointerTy() || !val ||
            val->getType()->isVoidTy()) {
            return;
        }
        builder_.CreateStore(val, dest_ptr, is_volatile);
    }
}

auto llvm_lowering::emit_get_element_ptr(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "GEP requires base and at least one index");
    auto* base_ptr{lower_value(inst.operands[0])};
    ASSERT(base_ptr, "GEP base pointer must be non-null");

    if (!base_ptr->getType()->isPointerTy()) {
        std::vector<u32> extract_indices;
        for (const auto& operand : inst.operands | std::views::drop(1)) {
            auto* idx{lower_value(operand)};
            if (auto* ci{llvm::dyn_cast_or_null<llvm::ConstantInt>(idx)}) {
                extract_indices.emplace_back(static_cast<u32>(ci->getZExtValue()));
            }
        }
        auto* extracted{builder_.CreateExtractValue(base_ptr, extract_indices, "extval")};
        if (inst.result) { set_local(*inst.result, extracted); }
        return extracted;
    }

    llvm::Type*               source_elem_ty{nullptr};
    std::vector<llvm::Value*> indices;

    if (inst.operands[0].type) {
        const auto& base_type{*inst.operands[0].type};
        switch (base_type.get_kind()) {
        case sema::type_kind::STRUCT:
        case sema::type_kind::UNION:
        case sema::type_kind::CLOSURE:
            source_elem_ty = types_.translate(base_type);
            indices.emplace_back(builder_.getInt32(0));
            for (const auto& operand : inst.operands | std::views::drop(1)) {
                auto* idx{lower_value(operand)};
                if (idx && idx->getType()->isIntegerTy(64)) {
                    idx = builder_.CreateIntCast(idx, types_.get_int32_ty(), false);
                }
                indices.emplace_back(idx);
            }
            break;
        case sema::type_kind::ARRAY:
            source_elem_ty = types_.translate(base_type);
            indices.emplace_back(builder_.getInt64(0));
            for (const auto& operand : inst.operands | std::views::drop(1)) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        case sema::type_kind::SLICE:
        case sema::type_kind::DYN:
            source_elem_ty = base_type.get_kind() == sema::type_kind::DYN
                                 ? static_cast<llvm::Type*>(types_.translate_dyn_fat_ptr())
                                 : types_.translate_slice_type();
            indices.emplace_back(builder_.getInt32(0));
            for (const auto& operand : inst.operands | std::views::drop(1)) {
                auto* idx{lower_value(operand)};
                if (idx && idx->getType()->isIntegerTy(64)) {
                    idx = builder_.CreateIntCast(idx, types_.get_int32_ty(), false);
                }
                indices.emplace_back(idx);
            }
            break;
        case sema::type_kind::POINTER: {
            if (const auto ptr_data{base_type.get_data().as_opt<sema::types::pointer>()}) {
                source_elem_ty = types_.translate(ptr_data->underlying);
            } else {
                source_elem_ty = types_.get_int8_ty();
            }

            for (const auto& operand : inst.operands | std::views::drop(1)) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        }
        case sema::type_kind::REFERENCE: {
            if (const auto ref_data{base_type.get_data().as_opt<sema::types::reference>()}) {
                source_elem_ty = types_.translate(ref_data->underlying);
            } else {
                source_elem_ty = types_.get_int8_ty();
            }

            for (const auto& operand : inst.operands | std::views::drop(1)) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        }
        default:
            if (inst.type) {
                source_elem_ty = types_.translate(*inst.type);
            } else {
                source_elem_ty = types_.get_int8_ty();
            }

            for (const auto& operand : inst.operands | std::views::drop(1)) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        }
    } else if (inst.type) {
        source_elem_ty = types_.translate(*inst.type);
        for (const auto& operand : inst.operands | std::views::drop(1)) {
            indices.emplace_back(lower_value(operand));
        }
    } else {
        source_elem_ty = types_.get_int8_ty();
        for (const auto& operand : inst.operands | std::views::drop(1)) {
            indices.emplace_back(lower_value(operand));
        }
    }

    auto* gep{builder_.CreateInBoundsGEP(source_elem_ty, base_ptr, indices, "geptmp")};
    if (inst.result) { set_local(*inst.result, gep); }
    return gep;
}

auto llvm_lowering::emit_address_of(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "AddressOf requires an operand");
    auto* val{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::emit_deref(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Deref requires an operand");
    auto* ptr{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, ptr); }
    return ptr;
}

auto llvm_lowering::emit_global_addr(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.callee_name, "global_addr requires the global's name");
    // Resolve by GIR name through `globals_` first to prevent ABI clash
    llvm::GlobalVariable* gv{nullptr};
    if (const auto it{globals_.find(*inst.callee_name)}; it != globals_.end()) {
        gv = llvm::dyn_cast<llvm::GlobalVariable>(it->second);
    }
    if (!gv) { gv = llvm_module_->getNamedGlobal(*inst.callee_name); }
    if (!gv) {
        for (const auto* g : gir_module_->get_globals()) {
            if (g->name == *inst.callee_name) {
                gv = lower_global(*g);
                break;
            }
        }
    }
    if (inst.result) { set_local(*inst.result, gv); }
    return gv;
}

auto llvm_lowering::emit_binary(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "Binary instruction requires at least 2 operands");
    auto* lhs{lower_value(inst.operands[0])};
    auto* rhs{lower_value(inst.operands[1])};
    ASSERT(lhs && rhs, "Binary operands must lower to non-null LLVM values");

    const bool is_flt{is_float_type(inst, lhs)};
    const bool is_sgn{is_signed_type(inst)};

    if (inst.is_checked && !is_flt) {
        if (auto* checked{emit_checked_arith(inst, lhs, rhs, is_sgn)}) { return checked; }
    }

    switch (inst.kind) {
    case gir::instruction_kind::ADD:
        return is_flt ? builder_.CreateFAdd(lhs, rhs, "addtmp")
                      : builder_.CreateAdd(lhs, rhs, "addtmp");
    case gir::instruction_kind::SUB:
        return is_flt ? builder_.CreateFSub(lhs, rhs, "subtmp")
                      : builder_.CreateSub(lhs, rhs, "subtmp");
    case gir::instruction_kind::MUL:
        return is_flt ? builder_.CreateFMul(lhs, rhs, "multmp")
                      : builder_.CreateMul(lhs, rhs, "multmp");
    case gir::instruction_kind::DIV:
        if (is_flt) { return builder_.CreateFDiv(lhs, rhs, "divtmp"); }
        return is_sgn ? builder_.CreateSDiv(lhs, rhs, "divtmp")
                      : builder_.CreateUDiv(lhs, rhs, "divtmp");
    case gir::instruction_kind::MOD:
        if (is_flt) { return builder_.CreateFRem(lhs, rhs, "modtmp"); }
        return is_sgn ? builder_.CreateSRem(lhs, rhs, "modtmp")
                      : builder_.CreateURem(lhs, rhs, "modtmp");
    case gir::instruction_kind::AND: return builder_.CreateAnd(lhs, rhs, "andtmp");
    case gir::instruction_kind::OR:  return builder_.CreateOr(lhs, rhs, "ortmp");
    case gir::instruction_kind::XOR: return builder_.CreateXor(lhs, rhs, "xortmp");
    case gir::instruction_kind::SHL: return builder_.CreateShl(lhs, rhs, "shltmp");
    case gir::instruction_kind::SHR:
        return is_sgn ? builder_.CreateAShr(lhs, rhs, "shrtmp")
                      : builder_.CreateLShr(lhs, rhs, "shrtmp");
    default: UNREACHABLE("Invalid instruction kind in emit_binary");
    }
}

auto llvm_lowering::emit_unary(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Unary instruction requires an operand");
    auto* val{lower_value(inst.operands[0])};
    ASSERT(val, "Unary operand must lower to a non-null LLVM value");

    const bool is_flt{is_float_type(inst, val)};

    if (inst.is_checked && !is_flt && inst.kind == gir::instruction_kind::NEG) {
        if (auto* int_ty{llvm::dyn_cast<llvm::IntegerType>(val->getType())}) {
            auto* int_min{llvm::ConstantInt::get(
                int_ty, llvm::APInt::getSignedMinValue(int_ty->getBitWidth()))};
            emit_arith_guard(builder_.CreateICmpEQ(val, int_min), "signed negation overflow", inst);
        }
    }

    switch (inst.kind) {
    case gir::instruction_kind::NEG:
        return is_flt ? builder_.CreateFNeg(val, "negtmp") : builder_.CreateNeg(val, "negtmp");
    case gir::instruction_kind::NOT:    return builder_.CreateNot(val, "nottmp");
    case gir::instruction_kind::BITNOT: return builder_.CreateNot(val, "bitnottmp");
    default:                            UNREACHABLE("Invalid instruction kind in emit_unary");
    }
}

auto llvm_lowering::emit_comparison(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "Comparison requires 2 operands");
    auto* lhs{lower_value(inst.operands[0])};
    auto* rhs{lower_value(inst.operands[1])};
    ASSERT(lhs && rhs, "Comparison operands must lower to non-null LLVM values");

    const bool is_flt{is_float_type(inst, lhs)};
    const bool is_sgn{is_signed_type(inst)};

    if (is_flt) {
        switch (inst.kind) {
        case gir::instruction_kind::EQ: return builder_.CreateFCmpOEQ(lhs, rhs, "cmptmp");
        case gir::instruction_kind::NE: return builder_.CreateFCmpONE(lhs, rhs, "cmptmp");
        case gir::instruction_kind::LT: return builder_.CreateFCmpOLT(lhs, rhs, "cmptmp");
        case gir::instruction_kind::LE: return builder_.CreateFCmpOLE(lhs, rhs, "cmptmp");
        case gir::instruction_kind::GT: return builder_.CreateFCmpOGT(lhs, rhs, "cmptmp");
        case gir::instruction_kind::GE: return builder_.CreateFCmpOGE(lhs, rhs, "cmptmp");
        default:                        UNREACHABLE("Invalid float comparison kind");
        }
    }

    if (is_sgn) {
        switch (inst.kind) {
        case gir::instruction_kind::EQ: return builder_.CreateICmpEQ(lhs, rhs, "cmptmp");
        case gir::instruction_kind::NE: return builder_.CreateICmpNE(lhs, rhs, "cmptmp");
        case gir::instruction_kind::LT: return builder_.CreateICmpSLT(lhs, rhs, "cmptmp");
        case gir::instruction_kind::LE: return builder_.CreateICmpSLE(lhs, rhs, "cmptmp");
        case gir::instruction_kind::GT: return builder_.CreateICmpSGT(lhs, rhs, "cmptmp");
        case gir::instruction_kind::GE: return builder_.CreateICmpSGE(lhs, rhs, "cmptmp");
        default:                        UNREACHABLE("Invalid signed comparison kind");
        }
    }

    switch (inst.kind) {
    case gir::instruction_kind::EQ: return builder_.CreateICmpEQ(lhs, rhs, "cmptmp");
    case gir::instruction_kind::NE: return builder_.CreateICmpNE(lhs, rhs, "cmptmp");
    case gir::instruction_kind::LT: return builder_.CreateICmpULT(lhs, rhs, "cmptmp");
    case gir::instruction_kind::LE: return builder_.CreateICmpULE(lhs, rhs, "cmptmp");
    case gir::instruction_kind::GT: return builder_.CreateICmpUGT(lhs, rhs, "cmptmp");
    case gir::instruction_kind::GE: return builder_.CreateICmpUGE(lhs, rhs, "cmptmp");
    default:                        UNREACHABLE("Invalid unsigned comparison kind");
    }
}

auto llvm_lowering::emit_cast(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Cast instruction requires an operand");
    ASSERT(inst.type, "Cast instruction requires a target type");
    auto* val{lower_value(inst.operands[0])};
    auto* target_ty{types_.translate(*inst.type)};
    ASSERT(val && target_ty, "Cast operand and target type must be valid");

    auto* src_ty{val->getType()};
    switch (inst.kind) {
    case gir::instruction_kind::WIDEN_CAST: {
        const bool src_is_flt{src_ty->isFloatingPointTy()};
        const bool dst_is_flt{target_ty->isFloatingPointTy()};

        if (src_is_flt && dst_is_flt) { return builder_.CreateFPCast(val, target_ty, "fpcast"); }
        if (!src_is_flt && dst_is_flt) {
            return is_signed_type(inst) ? builder_.CreateSIToFP(val, target_ty, "sitofp")
                                        : builder_.CreateUIToFP(val, target_ty, "uitofp");
        }
        if (src_is_flt && !dst_is_flt) {
            const bool dst_is_sgn{sema::is_signed_integer(*inst.type)};
            return dst_is_sgn ? builder_.CreateFPToSI(val, target_ty, "fptosi")
                              : builder_.CreateFPToUI(val, target_ty, "fptoui");
        }
        return builder_.CreateIntCast(val, target_ty, is_signed_type(inst));
    }
    case gir::instruction_kind::BIT_CAST: return builder_.CreateBitCast(val, target_ty, "bitcast");
    case gir::instruction_kind::PTR_CAST:
        return builder_.CreatePointerCast(val, target_ty, "ptrcast");
    case gir::instruction_kind::INT_FROM_PTR:
        return builder_.CreatePtrToInt(val, target_ty, "ptrtoint");
    case gir::instruction_kind::PTR_FROM_INT:
        return builder_.CreateIntToPtr(val, target_ty, "inttoptr");
    default: UNREACHABLE("Invalid cast instruction kind");
    }
}

auto llvm_lowering::emit_const(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Const instruction requires an operand");
    auto* val{lower_value(inst.operands[0], inst.type ? &*inst.type : nullptr)};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::resolve_named_function(std::string_view ghoti_name) -> llvm::Function* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(ghoti_name)}; it != globals_.end()) {
        if (auto* fn{llvm::dyn_cast<llvm::Function>(it->second)}) { return fn; }
    }
    return llvm_module_->getFunction(ghoti_name);
}

auto llvm_lowering::emit_call(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    if (inst.callee_name) {
        if (auto* callee_fn{resolve_named_function(*inst.callee_name)}) {
            std::vector<llvm::Value*> args;
            args.reserve(inst.operands.size());
            for (const auto& op : inst.operands) {
                if (op.type && op.type->get_kind() == sema::type_kind::TYPE) { continue; }
                auto* arg_val{lower_value(op)};
                if (!arg_val || arg_val->getType()->isVoidTy()) { continue; }
                if (op.type && op.type->get_kind() == sema::type_kind::ARRAY &&
                    args.size() < callee_fn->getFunctionType()->getNumParams() &&
                    callee_fn->getFunctionType()
                        ->getParamType(static_cast<u32>(args.size()))
                        ->isStructTy()) {
                    if (const auto arr_data{op.type->get_data().as_opt<sema::types::array>()}) {
                        auto*        slice_ty{types_.translate_slice_type()};
                        llvm::Value* slice_val{llvm::UndefValue::get(slice_ty)};
                        llvm::Value* ptr_val{arg_val};
                        if (!ptr_val->getType()->isPointerTy()) {
                            auto* tmp{
                                builder_.CreateAlloca(arg_val->getType(), nullptr, "arr.tmp")};
                            builder_.CreateStore(arg_val, tmp);
                            ptr_val = tmp;
                        }
                        slice_val = builder_.CreateInsertValue(
                            slice_val, ptr_val, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                        slice_val = builder_.CreateInsertValue(
                            slice_val,
                            builder_.getInt64(static_cast<u64>(arr_data->len)),
                            {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
                        arg_val = slice_val;
                    }
                } else if (op.type &&
                           (op.type->get_kind() == sema::type_kind::STRUCT ||
                            op.type->get_kind() == sema::type_kind::UNION ||
                            op.type->get_kind() == sema::type_kind::SLICE ||
                            op.type->get_kind() == sema::type_kind::CLOSURE) &&
                           arg_val->getType()->isPointerTy()) {
                    auto* llvm_struct_ty{types_.translate(*op.type)};
                    arg_val = builder_.CreateLoad(llvm_struct_ty, arg_val, "struct_arg");
                }
                args.emplace_back(arg_val);
            }
            const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_ ||
                               inst.type->get_kind() == sema::type_kind::NORETURN};

            auto* call_inst{builder_.CreateCall(callee_fn, args, is_void ? "" : "calltmp")};
            call_inst->setCallingConv(callee_fn->getCallingConv());
            if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
            return call_inst;
        }
    }

    ASSERT(!inst.operands.empty(), "Indirect call requires callee operand");
    auto* callee_val{lower_value(inst.operands[0])};
    ASSERT(inst.operands[0].type, "Indirect callee must have function type");
    auto ind_target{inst.operands[0].type};
    if (const auto ref{ind_target->get_data().as_opt<sema::types::reference>()}) {
        ind_target.emplace(ref->underlying);
    }
    if (const auto ptr{ind_target->get_data().as_opt<sema::types::pointer>()}) {
        ind_target.emplace(ptr->underlying);
    }
    const auto ind_fn_data{ind_target->get_data().as_opt<sema::types::function>()};
    ASSERT(ind_fn_data, "Indirect callee must have function type");
    auto* fn_ty{types_.translate_function_type(*ind_fn_data)};

    std::vector<llvm::Value*> args;
    args.reserve(inst.operands.size() - 1);
    for (const auto& operand : inst.operands | std::views::drop(1)) {
        auto* arg_val{lower_value(operand)};
        if (!arg_val || arg_val->getType()->isVoidTy()) { continue; }
        if (operand.type &&
            (operand.type->get_kind() == sema::type_kind::STRUCT ||
             operand.type->get_kind() == sema::type_kind::UNION ||
             operand.type->get_kind() == sema::type_kind::SLICE ||
             operand.type->get_kind() == sema::type_kind::CLOSURE) &&
            arg_val->getType()->isPointerTy()) {
            auto* llvm_struct_ty{types_.translate(*operand.type)};
            arg_val = builder_.CreateLoad(llvm_struct_ty, arg_val, "struct_arg");
        }
        args.emplace_back(arg_val);
    }

    const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_ ||
                       inst.type->get_kind() == sema::type_kind::NORETURN};
    auto*      call_inst{builder_.CreateCall(fn_ty, callee_val, args, is_void ? "" : "calltmp")};
    if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
    return call_inst;
}

auto llvm_lowering::emit_builtin_call(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    stdx::option<syntax::token_type_t> builtin_tok;
    if (inst.callee_name) {
        builtin_tok = syntax::get_builtin_opt(*inst.callee_name);
        if (!builtin_tok && !inst.callee_name->starts_with('@')) {
            const auto at_name{fmt::format("@{}", *inst.callee_name)};
            builtin_tok = syntax::get_builtin_opt(at_name);
        }
    }

    if (builtin_tok) {
        switch (*builtin_tok) {
        case syntax::token_type_t::BUILTIN_PANIC:
        case syntax::token_type_t::BUILTIN_TRAP:  {
            auto* trap_fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
            builder_.CreateCall(trap_fn, {});
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_EXPECT: {
            if (inst.operands.empty()) { return builder_.getInt1(true); }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return builder_.getInt1(true); }
            if (cond_val->getType()->isIntegerTy() &&
                cond_val->getType()->getIntegerBitWidth() != 1) {
                cond_val = builder_.CreateICmpNE(
                    cond_val, llvm::Constant::getNullValue(cond_val->getType()), "tobool");
            }

            auto* cur_fn{builder_.GetInsertBlock()->getParent()};
            auto* fail_bb{llvm::BasicBlock::Create(context_, "expect.fail", cur_fn)};
            auto* cont_bb{llvm::BasicBlock::Create(context_, "expect.cont", cur_fn)};

            builder_.CreateCondBr(cond_val, cont_bb, fail_bb);

            builder_.SetInsertPoint(fail_bb);
            auto* failed_flag{get_or_create_test_failed_flag()};
            builder_.CreateStore(builder_.getInt1(true), failed_flag);
            emit_context_handler_call(inst, "expect_handler", std::array{4UZ, 1UZ, 2UZ, 3UZ});
            builder_.CreateBr(cont_bb);

            builder_.SetInsertPoint(cont_bb);
            if (inst.result) { set_local(*inst.result, cond_val); }
            return cond_val;
        }
        case syntax::token_type_t::BUILTIN_REQUIRE: {
            if (inst.operands.empty()) { return nullptr; }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return nullptr; }
            if (cond_val->getType()->isIntegerTy() &&
                cond_val->getType()->getIntegerBitWidth() != 1) {
                cond_val = builder_.CreateICmpNE(
                    cond_val, llvm::Constant::getNullValue(cond_val->getType()), "tobool");
            }

            auto* cur_fn{builder_.GetInsertBlock()->getParent()};
            auto* fail_bb{llvm::BasicBlock::Create(context_, "require.fail", cur_fn)};
            auto* cont_bb{llvm::BasicBlock::Create(context_, "require.cont", cur_fn)};

            builder_.CreateCondBr(cond_val, cont_bb, fail_bb);

            builder_.SetInsertPoint(fail_bb);
            auto* failed_flag{get_or_create_test_failed_flag()};
            builder_.CreateStore(builder_.getInt1(true), failed_flag);
            emit_context_handler_call(inst, "require_handler", std::array{4UZ, 1UZ, 2UZ, 3UZ});
            builder_.CreateRet(builder_.getInt1(false));

            builder_.SetInsertPoint(cont_bb);
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_ASSERT:
        case syntax::token_type_t::BUILTIN_VERIFY: {
            const bool is_verify{*builtin_tok == syntax::token_type_t::BUILTIN_VERIFY};
            if (inst.operands.empty()) { return nullptr; }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return nullptr; }
            if (cond_val->getType()->isIntegerTy() &&
                cond_val->getType()->getIntegerBitWidth() != 1) {
                cond_val = builder_.CreateICmpNE(
                    cond_val, llvm::Constant::getNullValue(cond_val->getType()), "tobool");
            }

            auto* cur_fn{builder_.GetInsertBlock()->getParent()};
            auto* fail_bb{llvm::BasicBlock::Create(
                context_, is_verify ? "verify.fail" : "assert.fail", cur_fn)};
            auto* cont_bb{llvm::BasicBlock::Create(
                context_, is_verify ? "verify.cont" : "assert.cont", cur_fn)};
            builder_.CreateCondBr(cond_val, cont_bb, fail_bb);

            builder_.SetInsertPoint(fail_bb);
            if (is_verify) {
                std::string_view msg{"verification failed"};
                if (inst.operands.size() > 4) {
                    if (const auto s{inst.operands[4].as_opt<std::string>()}) { msg = *s; }
                }
                emit_lowered_panic(msg, inst);
                builder_.CreateUnreachable();
            } else {
                emit_context_handler_call(inst, "assert_handler", std::array{4UZ, 1UZ, 2UZ, 3UZ});
                builder_.CreateBr(cont_bb);
            }

            builder_.SetInsertPoint(cont_bb);
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_SKIP: {
            builder_.CreateStore(builder_.getInt1(true), get_or_create_test_skipped_flag());
            emit_context_handler_call(inst, "skip_handler", std::array{0UZ, 1UZ, 2UZ, 3UZ});
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_SRC: {
            ASSERT(inst.type, "@src must carry its SourceLocation result type");
            ASSERT(inst.operands.size() >= 3, "@src expects [file, line, column] operands");
            auto* struct_ty{llvm::cast<llvm::StructType>(types_.translate(*inst.type))};
            auto* slice_ty{types_.translate_slice_type()};

            llvm::Value* file_slice{llvm::UndefValue::get(slice_ty)};
            if (const auto path{inst.operands[0].as_opt<std::string>()}) {
                auto* gstr{builder_.CreateGlobalString(*path, "src.file")};
                file_slice = builder_.CreateInsertValue(
                    file_slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                file_slice =
                    builder_.CreateInsertValue(file_slice,
                                               builder_.getInt64(path->size()),
                                               {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
            }

            auto* line_v{lower_value(inst.operands[1])};
            auto* col_v{lower_value(inst.operands[2])};

            llvm::Value* agg{llvm::UndefValue::get(struct_ty)};
            agg = builder_.CreateInsertValue(agg, file_slice, {0U});
            if (line_v) { agg = builder_.CreateInsertValue(agg, line_v, {1U}); }
            if (col_v) { agg = builder_.CreateInsertValue(agg, col_v, {2U}); }
            return agg;
        }
        case syntax::token_type_t::BUILTIN_TARGET_OS: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{std::string{triple.getOSName()}, inst.type});
        }
        case syntax::token_type_t::BUILTIN_TARGET_ARCH: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{
                std::string{llvm::Triple::getArchTypeName(triple.getArch())}, inst.type});
        }
        case syntax::token_type_t::BUILTIN_TARGET_TRIPLE: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{triple.str(), inst.type});
        }

        // Memory operations
        case syntax::token_type_t::BUILTIN_MEMCPY: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* src{lower_value(inst.operands[1])};
            auto* len{lower_value(inst.operands[2])};
            if (dest && src && len) {
                builder_.CreateMemCpy(dest, llvm::MaybeAlign(), src, llvm::MaybeAlign(), len);
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_MEMSET: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            auto* len{lower_value(inst.operands[2])};
            if (dest && val && len) { builder_.CreateMemSet(dest, val, len, llvm::MaybeAlign()); }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_MEMMOVE: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* src{lower_value(inst.operands[1])};
            auto* len{lower_value(inst.operands[2])};
            if (dest && src && len) {
                builder_.CreateMemMove(dest, llvm::MaybeAlign(), src, llvm::MaybeAlign(), len);
            }
            return nullptr;
        }

        // C Varargs
        case syntax::token_type_t::BUILTIN_C_VA_START: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::vastart, {list->getType()})};
                builder_.CreateCall(fn, {list});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_C_VA_COPY: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* src{lower_value(inst.operands[1])};
            if (dest && src) {
                auto* fn{
                    llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(),
                                                            llvm::Intrinsic::vacopy,
                                                            {dest->getType(), src->getType()})};
                builder_.CreateCall(fn, {dest, src});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_C_VA_END: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::vaend, {list->getType()})};
                builder_.CreateCall(fn, {list});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_C_VA_ARG: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}; list && inst.type) {
                if (auto* ty{types_.translate(*inst.type)}) {
                    return builder_.CreateVAArg(list, ty, "vaarg");
                }
            }
            return nullptr;
        }

        // Bit operations
        case syntax::token_type_t::BUILTIN_CLZ: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::ctlz, {val->getType()})};
                return builder_.CreateCall(fn, {val, builder_.getInt1(false)});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_CTZ: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::cttz, {val->getType()})};
                return builder_.CreateCall(fn, {val, builder_.getInt1(false)});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_POP_COUNT: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::ctpop, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }

        // Fused multiply add
        case syntax::token_type_t::BUILTIN_MUL_ADD: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            const usize base_idx{inst.operands.size() >= 4 ? 1u : 0u};
            auto*       a{lower_value(inst.operands[base_idx])};
            auto*       b{lower_value(inst.operands[base_idx + 1])};
            auto*       c{lower_value(inst.operands[base_idx + 2])};
            if (a && b && c) {
                if (a->getType()->isFloatingPointTy()) {
                    auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                        llvm_module_.get(), llvm::Intrinsic::fmuladd, {a->getType()})};
                    return builder_.CreateCall(fn, {a, b, c});
                }
                auto* mul{builder_.CreateMul(a, b, "mul")};
                return builder_.CreateAdd(mul, c, "add");
            }
            return nullptr;
        }

        case syntax::token_type_t::BUILTIN_ABS: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                if (val->getType()->isFloatingPointTy()) {
                    auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                        llvm_module_.get(), llvm::Intrinsic::fabs, {val->getType()})};
                    return builder_.CreateCall(fn, {val});
                }
                if (val->getType()->isIntegerTy()) {
                    auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                        llvm_module_.get(), llvm::Intrinsic::abs, {val->getType()})};
                    return builder_.CreateCall(fn, {val, builder_.getInt1(false)});
                }
            }
            return nullptr;
        }

        case syntax::token_type_t::BUILTIN_FIELD_PARENT_PTR: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* field_ptr{lower_value(inst.operands[0])};
            if (!field_ptr || !inst.type) { return nullptr; }
            const auto ptr_data{inst.type->get_data().as_opt<sema::types::pointer>()};
            if (!ptr_data) { return nullptr; }
            auto* struct_ty{types_.translate(ptr_data->underlying)};
            if (!struct_ty || !struct_ty->isStructTy()) { return nullptr; }

            auto* idx_val{lower_value(inst.operands[1])};
            auto* idx_const{llvm::dyn_cast_or_null<llvm::ConstantInt>(idx_val)};
            if (!idx_const) { return nullptr; }
            const auto  field_idx{idx_const->getZExtValue()};
            const auto& layout{llvm_module_->getDataLayout()};
            const auto  offset{layout.getStructLayout(llvm::cast<llvm::StructType>(struct_ty))
                                  ->getElementOffset(static_cast<u32>(field_idx))};

            auto* field_int{builder_.CreatePtrToInt(field_ptr, types_.get_int64_ty(), "fpp.i")};
            auto* parent_int{builder_.CreateSub(field_int, builder_.getInt64(offset), "fpp.base")};
            return builder_.CreateIntToPtr(parent_int, types_.get_ptr_ty(), "fpp.p");
        }

        case syntax::token_type_t::BUILTIN_MIN:
        case syntax::token_type_t::BUILTIN_MAX: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_max{*builtin_tok == syntax::token_type_t::BUILTIN_MAX};
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            if (a->getType()->isFloatingPointTy()) {
                auto  id{is_max ? llvm::Intrinsic::maxnum : llvm::Intrinsic::minnum};
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), id, {a->getType()})};
                return builder_.CreateCall(fn, {a, b});
            }
            auto  id{is_max ? (is_signed ? llvm::Intrinsic::smax : llvm::Intrinsic::umax)
                            : (is_signed ? llvm::Intrinsic::smin : llvm::Intrinsic::umin)};
            auto* fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, {a->getType()})};
            return builder_.CreateCall(fn, {a, b});
        }

        case syntax::token_type_t::BUILTIN_DIV_TRUNC:
        case syntax::token_type_t::BUILTIN_REM:       {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            const bool is_rem{*builtin_tok == syntax::token_type_t::BUILTIN_REM};
            if (is_rem) {
                return is_signed ? builder_.CreateSRem(a, b) : builder_.CreateURem(a, b);
            }
            return is_signed ? builder_.CreateSDiv(a, b) : builder_.CreateUDiv(a, b);
        }

        case syntax::token_type_t::BUILTIN_DIV_FLOOR:
        case syntax::token_type_t::BUILTIN_MOD:       {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            const bool is_mod{*builtin_tok == syntax::token_type_t::BUILTIN_MOD};
            if (!is_signed) {
                return is_mod ? builder_.CreateURem(a, b) : builder_.CreateUDiv(a, b);
            }
            auto* zero{llvm::ConstantInt::get(a->getType(), 0)};
            if (is_mod) {
                // r = a %s b; ((r != 0) && ((r < 0) != (b < 0))) ? r + b : r
                auto* r{builder_.CreateSRem(a, b)};
                auto* r_nonzero{builder_.CreateICmpNE(r, zero)};
                auto* signs_differ{builder_.CreateICmpNE(builder_.CreateICmpSLT(r, zero),
                                                         builder_.CreateICmpSLT(b, zero))};
                auto* adjust{builder_.CreateAnd(r_nonzero, signs_differ)};
                return builder_.CreateSelect(adjust, builder_.CreateAdd(r, b), r);
            }
            // q = a /s b; ((a %s b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q
            auto* q{builder_.CreateSDiv(a, b)};
            auto* r{builder_.CreateSRem(a, b)};
            auto* r_nonzero{builder_.CreateICmpNE(r, zero)};
            auto* signs_differ{builder_.CreateICmpNE(builder_.CreateICmpSLT(a, zero),
                                                     builder_.CreateICmpSLT(b, zero))};
            auto* adjust{builder_.CreateAnd(r_nonzero, signs_differ)};
            return builder_.CreateSelect(
                adjust, builder_.CreateSub(q, llvm::ConstantInt::get(a->getType(), 1)), q);
        }

        case syntax::token_type_t::BUILTIN_ADD_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_SUB_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_MUL_WITH_OVERFLOW:
        case syntax::token_type_t::BUILTIN_SHL_WITH_OVERFLOW: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            auto* out_ptr{lower_value(inst.operands[2])};
            if (!a || !b || !out_ptr) { return nullptr; }
            const bool is_signed{inst.operands[0].type &&
                                 sema::is_signed_integer(*inst.operands[0].type)};

            llvm::Value* result{nullptr};
            llvm::Value* overflow{nullptr};
            if (*builtin_tok == syntax::token_type_t::BUILTIN_SHL_WITH_OVERFLOW) {
                result = builder_.CreateShl(a, b);
                auto* back{is_signed ? builder_.CreateAShr(result, b)
                                     : builder_.CreateLShr(result, b)};
                overflow = builder_.CreateICmpNE(back, a);
            } else {
                llvm::Intrinsic::ID id{};
                switch (*builtin_tok) {
                case syntax::token_type_t::BUILTIN_ADD_WITH_OVERFLOW:
                    id = is_signed ? llvm::Intrinsic::sadd_with_overflow
                                   : llvm::Intrinsic::uadd_with_overflow;
                    break;
                case syntax::token_type_t::BUILTIN_SUB_WITH_OVERFLOW:
                    id = is_signed ? llvm::Intrinsic::ssub_with_overflow
                                   : llvm::Intrinsic::usub_with_overflow;
                    break;
                default:
                    id = is_signed ? llvm::Intrinsic::smul_with_overflow
                                   : llvm::Intrinsic::umul_with_overflow;
                    break;
                }
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), id, {a->getType()})};
                auto* pair{builder_.CreateCall(fn, {a, b})};
                result   = builder_.CreateExtractValue(pair, {0U});
                overflow = builder_.CreateExtractValue(pair, {1U});
            }
            builder_.CreateStore(result, out_ptr);
            return overflow;
        }

        case syntax::token_type_t::BUILTIN_ATOMIC_LOAD: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            if (!ptr || !inst.type) { return nullptr; }
            auto* load{builder_.CreateLoad(types_.translate(*inst.type), ptr)};
            load->setAtomic(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return load;
        }
        case syntax::token_type_t::BUILTIN_ATOMIC_STORE: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            if (!ptr || !val) { return nullptr; }
            auto* store{builder_.CreateStore(val, ptr)};
            store->setAtomic(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_ATOMIC_RMW: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            if (!ptr || !val) { return nullptr; }
            return builder_.CreateAtomicRMW(to_llvm_rmw_op(inst.atomic_op.value_or(0)),
                                            ptr,
                                            val,
                                            llvm::MaybeAlign(),
                                            to_llvm_ordering(inst.atomic_order.value_or(0)));
        }
        case syntax::token_type_t::BUILTIN_CMPXCHG_WEAK:
        case syntax::token_type_t::BUILTIN_CMPXCHG_STRONG: {
            VERIFY(inst.operands.size() >= 4, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* expected{lower_value(inst.operands[1])};
            auto* new_val{lower_value(inst.operands[2])};
            auto* out_ptr{lower_value(inst.operands[3])};
            if (!ptr || !expected || !new_val || !out_ptr) { return nullptr; }
            auto* cmpxchg{
                builder_.CreateAtomicCmpXchg(ptr,
                                             expected,
                                             new_val,
                                             llvm::MaybeAlign(),
                                             to_llvm_ordering(inst.atomic_order.value_or(0)),
                                             to_llvm_ordering(inst.atomic_fail_order.value_or(0)))};
            cmpxchg->setWeak(*builtin_tok == syntax::token_type_t::BUILTIN_CMPXCHG_WEAK);
            auto* cx_result{builder_.CreateExtractValue(cmpxchg, {0U})};
            auto* success{builder_.CreateExtractValue(cmpxchg, {1U})};
            builder_.CreateStore(cx_result, out_ptr);
            return success;
        }
        case syntax::token_type_t::BUILTIN_FENCE: {
            builder_.CreateFence(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return nullptr;
        }

        default: break;
        }
    }
    return emit_call(inst);
}

auto llvm_lowering::emit_inline_asm(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.asm_info, "INLINE_ASM instruction requires asm_info");
    const auto& info{*inst.asm_info};

    // Input operands become the asm call's arguments, in listed order.
    std::vector<llvm::Value*> args;
    std::vector<llvm::Type*>  param_tys;
    args.reserve(inst.operands.size());
    param_tys.reserve(inst.operands.size());
    for (const auto& op : inst.operands) {
        auto* val{lower_value(op)};
        ASSERT(val, "inline asm input operand lowered to null");
        args.emplace_back(val);
        param_tys.emplace_back(val->getType());
    }

    // One result per bound output operand, or a single result for the `_` result slot.
    std::vector<llvm::Type*> result_tys;
    if (info.has_result_slot) {
        ASSERT(inst.type, "inline asm result slot requires a result type");
        result_tys.emplace_back(types_.translate(*inst.type));
    } else {
        for (const auto& addr : info.output_addrs) {
            ASSERT(addr.type, "inline asm output operand requires a type");
            result_tys.emplace_back(types_.translate(*addr.type));
        }
    }

    llvm::Type* ret_ty{nullptr};
    if (result_tys.empty()) {
        ret_ty = types_.get_void_ty();
    } else if (result_tys.size() == 1) {
        ret_ty = result_tys.front();
    } else {
        ret_ty = llvm::StructType::get(context_, result_tys);
    }

    auto*      fn_ty{llvm::FunctionType::get(ret_ty, param_tys, false)};
    const bool has_side_effects{info.is_volatile || result_tys.empty()};
    const auto dialect{info.intel_dialect ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT};

    auto* inline_asm{llvm::InlineAsm::get(
        fn_ty, info.tmpl, info.constraints, has_side_effects, info.align_stack, dialect, false)};

    auto* call{builder_.CreateCall(fn_ty, inline_asm, args)};
    call->addFnAttr(llvm::Attribute::NoUnwind);
    if (info.is_noreturn) { call->setDoesNotReturn(); }

    // Store each bound output back through its target address.
    if (!info.has_result_slot && !info.output_addrs.empty()) {
        if (info.output_addrs.size() == 1) {
            builder_.CreateStore(call, lower_value(info.output_addrs.front()));
        } else {
            for (u32 i{0}; i < info.output_addrs.size(); ++i) {
                auto* field{builder_.CreateExtractValue(call, i)};
                builder_.CreateStore(field, lower_value(info.output_addrs[i]));
            }
        }
    }

    return info.has_result_slot ? call : nullptr;
}

auto llvm_lowering::emit_ret(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    if (inst.operands.empty() || (inst.type && inst.type->get_kind() == sema::type_kind::VOID_)) {
        builder_.CreateRetVoid();
        return;
    }

    auto* val{lower_value(inst.operands[0])};
    if (!val || val->getType()->isVoidTy()) {
        builder_.CreateRetVoid();
        return;
    }

    auto* fn{builder_.GetInsertBlock() ? builder_.GetInsertBlock()->getParent() : nullptr};
    auto* ret_ty{fn ? fn->getReturnType()
                    : (inst.type ? types_.translate(*inst.type) : val->getType())};

    if (val->getType() != ret_ty) {
        if (val->getType()->isPointerTy() && !ret_ty->isPointerTy()) {
            val = builder_.CreateLoad(ret_ty, val, "retval");
        } else if (val->getType()->isIntegerTy() && ret_ty->isIntegerTy()) {
            val = builder_.CreateIntCast(val, ret_ty, is_signed_type(inst));
        }
    }

    builder_.CreateRet(val);
}

auto llvm_lowering::emit_goto(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    ASSERT(inst.target_segment, "GOTO instruction requires a target segment");
    const auto it{segment_blocks_.find(*inst.target_segment)};
    ASSERT(it != segment_blocks_.end(), "Target segment block not found");
    builder_.CreateBr(it->second);
}

auto llvm_lowering::emit_cond_goto(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    ASSERT(inst.true_segment && inst.false_segment,
           "COND_GOTO requires true and false target segments");
    ASSERT(!inst.operands.empty(), "COND_GOTO requires condition operand");

    auto*      cond_val{lower_value(inst.operands[0])};
    const auto true_it{segment_blocks_.find(*inst.true_segment)};
    const auto false_it{segment_blocks_.find(*inst.false_segment)};
    ASSERT(true_it != segment_blocks_.end() && false_it != segment_blocks_.end(),
           "COND_GOTO branch target blocks not found");
    builder_.CreateCondBr(cond_val, true_it->second, false_it->second);
}

auto llvm_lowering::emit_unreachable() -> void { builder_.CreateUnreachable(); }

} // namespace ghoti::codegen
