#include "compiler/codegen/aggregate_abi.hh"

#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

namespace ghoti::codegen {

namespace {

// Past this, a by-value aggregate costs more than a pointer to it (and eventually cannot be selected)
constexpr u64 max_direct_aggregate_bytes{256};

class aggregate_lowering {
  public:
    explicit aggregate_lowering(llvm::Module& module)
        : module_{module}, layout_{module.getDataLayout()}, context_{module.getContext()} {}

    auto run() -> bool {
        rewrite_signatures();
        rewrite_calls();
        rewrite_copies();
        return memcpy_used_;
    }

  private:
    // How a signature changes: large parameters become pointers, a large result an `sret` slot
    struct abi_shape {
        llvm::FunctionType*      lowered{nullptr};
        llvm::Type*              sret_type{nullptr};
        std::vector<llvm::Type*> indirect_params; // the aggregate passed by pointer, or null
    };

    [[nodiscard]] auto is_large(llvm::Type* ty) const -> bool {
        return ty->isAggregateType() && ty->isSized() &&
               layout_.getTypeAllocSize(ty).getFixedValue() > max_direct_aggregate_bytes;
    }

    [[nodiscard]] auto shape_of(llvm::FunctionType* fn_ty) const -> stdx::option<abi_shape> {
        abi_shape                shape;
        std::vector<llvm::Type*> params;
        auto*                    ret_ty{fn_ty->getReturnType()};
        auto*                    ptr_ty{llvm::PointerType::get(context_, 0)};
        if (is_large(ret_ty)) {
            shape.sret_type = ret_ty;
            ret_ty          = llvm::Type::getVoidTy(context_);
            params.emplace_back(ptr_ty);
        }
        bool any_indirect{false};
        for (auto* param : fn_ty->params()) {
            const bool indirect{is_large(param)};
            any_indirect = any_indirect || indirect;
            shape.indirect_params.emplace_back(indirect ? param : nullptr);
            params.emplace_back(indirect ? ptr_ty : param);
        }
        if (!shape.sret_type && !any_indirect) { return stdx::none; }
        shape.lowered = llvm::FunctionType::get(ret_ty, params, fn_ty->isVarArg());
        return shape;
    }

    auto copy_bytes(llvm::IRBuilder<>& builder, llvm::Value* dest, llvm::Value* src, llvm::Type* ty)
        -> void {
        builder.CreateMemCpy(dest,
                             llvm::MaybeAlign(),
                             src,
                             llvm::MaybeAlign(),
                             layout_.getTypeAllocSize(ty).getFixedValue());
        memcpy_used_ = true;
    }

    [[nodiscard]] auto spill_constant(llvm::Constant* constant) -> llvm::GlobalVariable* {
        auto* global{new llvm::GlobalVariable{module_,
                                              constant->getType(),
                                              true,
                                              llvm::GlobalValue::PrivateLinkage,
                                              constant,
                                              ".agg.init"}};
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        return global;
    }

    // Nothing between `from` and `to` in one block can have written memory
    [[nodiscard]] static auto memory_unchanged_between(const llvm::Instruction* from,
                                                       const llvm::Instruction* to) -> bool {
        if (from->getParent() != to->getParent()) { return false; }
        for (const auto* inst{from->getNextNode()}; inst && inst != to; inst = inst->getNextNode()) {
            if (inst->mayWriteToMemory()) { return false; }
        }
        return true;
    }

    // Where `value`'s bytes can be copied from at `at`: a spilled constant, or the address of a
    // load whose source nothing has overwritten since
    [[nodiscard]] auto source_address(llvm::Value* value, const llvm::Instruction* at)
        -> llvm::Value* {
        if (auto* constant{llvm::dyn_cast<llvm::Constant>(value)}) {
            return spill_constant(constant);
        }
        auto* load{llvm::dyn_cast<llvm::LoadInst>(value)};
        if (!load || load->isVolatile() || !memory_unchanged_between(load, at)) { return nullptr; }
        return load->getPointerOperand();
    }

    static auto erase_if_dead(llvm::Value* value) -> void {
        if (auto* inst{llvm::dyn_cast_or_null<llvm::Instruction>(value)};
            inst && inst->use_empty() && !inst->mayHaveSideEffects()) {
            inst->eraseFromParent();
        }
    }

    [[nodiscard]] static auto entry_alloca(llvm::Function& fn, llvm::Type* ty) -> llvm::AllocaInst* {
        llvm::IRBuilder<> builder{&*fn.getEntryBlock().getFirstInsertionPt()};
        return builder.CreateAlloca(ty, nullptr, "agg.tmp");
    }

    // Uses of an old by-value parameter now read through its pointer
    auto redirect_indirect_param(llvm::Argument& old_arg, llvm::Argument& new_ptr, llvm::Type* ty)
        -> void {
        for (auto* user : llvm::make_early_inc_range(old_arg.users())) {
            auto* store{llvm::dyn_cast<llvm::StoreInst>(user)};
            if (!store || store->isVolatile() || store->getValueOperand() != &old_arg) { continue; }
            llvm::IRBuilder<> builder{store};
            copy_bytes(builder, store->getPointerOperand(), &new_ptr, ty);
            store->eraseFromParent();
        }
        if (old_arg.use_empty()) { return; }
        llvm::IRBuilder<> builder{&*new_ptr.getParent()->getEntryBlock().getFirstInsertionPt()};
        old_arg.replaceAllUsesWith(builder.CreateLoad(ty, &new_ptr, "agg.param"));
    }

    auto redirect_returns(llvm::Function& fn, llvm::Value* sret, llvm::Type* ty) -> void {
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

    auto rewrite_signatures() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::Function*> candidates;
        for (auto& fn : module_) {
            if (!fn.isIntrinsic() && shape_of(fn.getFunctionType())) { candidates.push_back(&fn); }
        }

        for (auto* old_fn : candidates) {
            const auto shape{*shape_of(old_fn->getFunctionType())};
            auto*      new_fn{llvm::Function::Create(
                shape.lowered, old_fn->getLinkage(), old_fn->getAddressSpace(), "", &module_)};
            new_fn->copyAttributesFrom(old_fn);
            new_fn->setComdat(old_fn->getComdat());
            // Parameter attributes no longer line up with the rewritten parameter list
            new_fn->setAttributes(llvm::AttributeList::get(
                context_, old_fn->getAttributes().getFnAttrs(), llvm::AttributeSet{}, {}));
            new_fn->takeName(old_fn);

            const unsigned first_param{shape.sret_type ? 1U : 0U};
            if (shape.sret_type) {
                new_fn->addParamAttr(
                    0, llvm::Attribute::getWithStructRetType(context_, shape.sret_type));
            }

            if (!old_fn->isDeclaration()) {
                new_fn->splice(new_fn->end(), old_fn);
                for (auto& old_arg : old_fn->args()) {
                    const auto idx{old_arg.getArgNo()};
                    auto&      new_arg{*new_fn->getArg(idx + first_param)};
                    new_arg.takeName(&old_arg);
                    if (auto* aggregate{shape.indirect_params[idx]}) {
                        redirect_indirect_param(old_arg, new_arg, aggregate);
                    } else {
                        old_arg.replaceAllUsesWith(&new_arg);
                    }
                }
                if (shape.sret_type) {
                    redirect_returns(*new_fn, new_fn->getArg(0), shape.sret_type);
                }
            }

            old_fn->replaceAllUsesWith(new_fn);
            old_fn->eraseFromParent();
        }
    }

    // Copies a by-value argument into its caller-owned pointer slot
    auto materialize_argument(llvm::Value* arg, llvm::Value* slot, llvm::Type* ty, llvm::CallInst& call)
        -> void {
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

    auto redirect_sret_result(llvm::CallInst& call, llvm::Value* slot, llvm::Type* ty) -> void {
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

    auto rewrite_calls() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::CallInst*> calls;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                auto* call{llvm::dyn_cast<llvm::CallInst>(&inst)};
                if (!call) { continue; }
                if (const auto* callee{call->getCalledFunction()}; callee && callee->isIntrinsic()) {
                    continue;
                }
                if (shape_of(call->getFunctionType())) { calls.push_back(call); }
            }
        }

        for (auto* call : calls) {
            const auto shape{*shape_of(call->getFunctionType())};
            auto&      caller{*call->getFunction()};

            std::vector<llvm::Value*> args;
            llvm::Value*              sret_slot{nullptr};
            if (shape.sret_type) {
                sret_slot = entry_alloca(caller, shape.sret_type);
                args.push_back(sret_slot);
            }
            std::vector<llvm::Value*> copied_from;
            for (unsigned idx{0}; idx < call->arg_size(); ++idx) {
                auto* arg{call->getArgOperand(idx)};
                auto* aggregate{idx < shape.indirect_params.size() ? shape.indirect_params[idx]
                                                                   : nullptr};
                if (!aggregate) {
                    args.push_back(arg);
                    continue;
                }
                auto* slot{entry_alloca(caller, aggregate)};
                materialize_argument(arg, slot, aggregate, *call);
                args.push_back(slot);
                copied_from.push_back(arg);
            }

            llvm::IRBuilder<> builder{call};
            auto*             lowered{builder.CreateCall(shape.lowered, call->getCalledOperand(), args)};
            lowered->setCallingConv(call->getCallingConv());
            lowered->setAttributes(llvm::AttributeList::get(
                context_, call->getAttributes().getFnAttrs(), llvm::AttributeSet{}, {}));
            if (shape.sret_type) {
                lowered->addParamAttr(
                    0, llvm::Attribute::getWithStructRetType(context_, shape.sret_type));
                redirect_sret_result(*call, sret_slot, shape.sret_type);
            } else if (!call->getType()->isVoidTy()) {
                lowered->takeName(call);
                call->replaceAllUsesWith(lowered);
            }
            call->eraseFromParent();
            for (auto* arg : copied_from) { erase_if_dead(arg); }
        }
    }

    // Any remaining whole-aggregate store of a large value becomes a byte copy
    auto rewrite_copies() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::StoreInst*> stores;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                auto* store{llvm::dyn_cast<llvm::StoreInst>(&inst)};
                if (store && !store->isVolatile() && is_large(store->getValueOperand()->getType())) {
                    stores.push_back(store);
                }
            }
        }
        for (auto* store : stores) {
            auto* value{store->getValueOperand()};
            if (llvm::isa<llvm::UndefValue>(value)) {
                store->eraseFromParent();
                continue;
            }
            auto* src{source_address(value, store)};
            if (!src) { continue; }
            llvm::IRBuilder<> builder{store};
            copy_bytes(builder, store->getPointerOperand(), src, value->getType());
            store->eraseFromParent();
            erase_if_dead(value);
        }
    }

  private:
    llvm::Module&           module_;
    const llvm::DataLayout& layout_;
    llvm::LLVMContext&      context_;
    bool                    memcpy_used_{false};
};

} // namespace

auto lower_large_aggregates(llvm::Module& module) -> bool {
    PROFILE_FUNCTION();
    return aggregate_lowering{module}.run();
}

} // namespace ghoti::codegen
