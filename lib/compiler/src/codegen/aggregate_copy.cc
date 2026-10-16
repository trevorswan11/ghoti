#include "compiler/codegen/aggregate_copy.hh"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Casting.h>

namespace ghoti::codegen {

namespace {

// Nothing between `from` and `to` in one block can have written memory
[[nodiscard]] auto memory_unchanged_between(const llvm::Instruction* from,
                                            const llvm::Instruction* to) -> bool {
    if (from->getParent() != to->getParent()) { return false; }
    for (const auto* inst{from->getNextNode()}; inst && inst != to; inst = inst->getNextNode()) {
        if (inst->mayWriteToMemory()) { return false; }
    }
    return true;
}

} // namespace

auto aggregate_copier::copy_bytes(llvm::IRBuilder<>& builder,
                                  llvm::Value*       dest,
                                  llvm::Value*       src,
                                  llvm::Type*        ty) -> void {
    builder.CreateMemCpy(dest,
                         llvm::MaybeAlign(),
                         src,
                         llvm::MaybeAlign(),
                         layout_.getTypeAllocSize(ty).getFixedValue());
    memcpy_used_ = true;
}

auto aggregate_copier::spill_constant(llvm::Constant* constant) -> llvm::GlobalVariable* {
    auto* global{new llvm::GlobalVariable{module_,
                                          constant->getType(),
                                          true,
                                          llvm::GlobalValue::PrivateLinkage,
                                          constant,
                                          ".agg.init"}};
    global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    return global;
}

auto aggregate_copier::source_address(llvm::Value* value, const llvm::Instruction* at)
    -> llvm::Value* {
    if (auto* constant{llvm::dyn_cast<llvm::Constant>(value)}) { return spill_constant(constant); }
    auto* load{llvm::dyn_cast<llvm::LoadInst>(value)};
    if (!load || load->isVolatile() || !memory_unchanged_between(load, at)) { return nullptr; }
    return load->getPointerOperand();
}

auto aggregate_copier::erase_if_dead(llvm::Value* value) -> void {
    if (auto* inst{llvm::dyn_cast_or_null<llvm::Instruction>(value)};
        inst && inst->use_empty() && !inst->mayHaveSideEffects()) {
        inst->eraseFromParent();
    }
}

auto aggregate_copier::entry_alloca(llvm::Function& fn, llvm::Type* ty) -> llvm::AllocaInst* {
    llvm::IRBuilder<> builder{&*fn.getEntryBlock().getFirstInsertionPt()};
    return builder.CreateAlloca(ty, nullptr, "agg.tmp");
}

auto aggregate_copier::redirect_indirect_param(llvm::Argument& old_arg,
                                               llvm::Argument& pointer,
                                               llvm::Type*     ty) -> void {
    for (auto* user : llvm::make_early_inc_range(old_arg.users())) {
        auto* store{llvm::dyn_cast<llvm::StoreInst>(user)};
        if (!store || store->isVolatile() || store->getValueOperand() != &old_arg) { continue; }
        llvm::IRBuilder<> builder{store};
        copy_bytes(builder, store->getPointerOperand(), &pointer, ty);
        store->eraseFromParent();
    }
    if (old_arg.use_empty()) { return; }
    llvm::IRBuilder<> builder{&*pointer.getParent()->getEntryBlock().getFirstInsertionPt()};
    old_arg.replaceAllUsesWith(builder.CreateLoad(ty, &pointer, "agg.param"));
}

auto aggregate_copier::redirect_returns(llvm::Function& fn, llvm::Value* sret, llvm::Type* ty)
    -> void {
    for (auto& block : fn) {
        auto* ret{llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())};
        if (!ret || !ret->getReturnValue()) { continue; }
        auto*             value{ret->getReturnValue()};
        llvm::IRBuilder<> builder{ret};
        if (!llvm::isa<llvm::UndefValue>(value)) {
            if (auto* src{source_address(value, ret)}) {
                copy_bytes(builder, sret, src, ty);
            } else {
                builder.CreateStore(value, sret);
            }
        }
        builder.CreateRetVoid();
        ret->eraseFromParent();
        erase_if_dead(value);
    }
}

auto aggregate_copier::materialize_argument(llvm::Value*    arg,
                                            llvm::Value*    slot,
                                            llvm::Type*     ty,
                                            llvm::CallInst& call) -> void {
    // Snapshot a loaded argument where it was read, so later argument evaluation cannot race it
    if (auto* load{llvm::dyn_cast<llvm::LoadInst>(arg)}; load && !load->isVolatile()) {
        llvm::IRBuilder<> builder{load->getNextNode()};
        copy_bytes(builder, slot, load->getPointerOperand(), ty);
        return;
    }
    llvm::IRBuilder<> builder{&call};
    if (auto* constant{llvm::dyn_cast<llvm::Constant>(arg)}) {
        copy_bytes(builder, slot, spill_constant(constant), ty);
    } else {
        builder.CreateStore(arg, slot);
    }
}

auto aggregate_copier::redirect_sret_result(llvm::CallInst& call, llvm::Value* slot, llvm::Type* ty)
    -> void {
    for (auto* user : llvm::make_early_inc_range(call.users())) {
        auto* store{llvm::dyn_cast<llvm::StoreInst>(user)};
        if (!store || store->isVolatile() || store->getValueOperand() != &call) { continue; }
        llvm::IRBuilder<> builder{store};
        copy_bytes(builder, store->getPointerOperand(), slot, ty);
        store->eraseFromParent();
    }
    if (call.use_empty()) { return; }
    llvm::IRBuilder<> builder{call.getNextNode()};
    call.replaceAllUsesWith(builder.CreateLoad(ty, slot, "agg.result"));
}

} // namespace ghoti::codegen
