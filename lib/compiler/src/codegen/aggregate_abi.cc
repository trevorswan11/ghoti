#include "compiler/codegen/aggregate_abi.hh"

#include <algorithm>
#include <vector>

#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/codegen/aggregate_copy.hh"

namespace ghoti::codegen {

namespace {

// Past this, a by-value aggregate costs more than a pointer to it
constexpr u64 max_direct_aggregate_bytes{256};

class aggregate_lowering {
  public:
    explicit aggregate_lowering(llvm::Module& module)
        : module_{module}, layout_{module.getDataLayout()}, context_{module.getContext()},
          copier_{module} {}

    auto run() -> bool {
        rewrite_signatures();
        rewrite_calls();
        rewrite_copies();
        return copier_.used_memcpy();
    }

  private:
    // How a signature changes: large parameters become pointers, a large result an `sret` slot
    struct abi_shape {
        llvm::FunctionType*      lowered{nullptr};
        llvm::Type*              sret_type{nullptr};
        std::vector<llvm::Type*> indirect_params; // the aggregate passed by pointer, or null
    };

  private:
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

    auto rewrite_signatures() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::Function*> candidates;
        for (auto& fn : module_) {
            if (!fn.isIntrinsic() && shape_of(fn.getFunctionType())) {
                candidates.emplace_back(&fn);
            }
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

            const u32 first_param{shape.sret_type ? 1U : 0U};
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
                        copier_.redirect_indirect_param(old_arg, new_arg, aggregate);
                    } else {
                        old_arg.replaceAllUsesWith(&new_arg);
                    }
                }
                if (shape.sret_type) {
                    copier_.redirect_returns(*new_fn, new_fn->getArg(0), shape.sret_type);
                }
            }

            old_fn->replaceAllUsesWith(new_fn);
            old_fn->eraseFromParent();
        }
    }

    auto rewrite_calls() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::CallInst*> calls;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                auto* call{llvm::dyn_cast<llvm::CallInst>(&inst)};
                if (!call) { continue; }
                if (const auto* callee{call->getCalledFunction()};
                    callee && callee->isIntrinsic()) {
                    continue;
                }
                if (shape_of(call->getFunctionType())) { calls.emplace_back(call); }
            }
        }

        for (auto* call : calls) {
            const auto shape{*shape_of(call->getFunctionType())};
            auto&      caller{*call->getFunction()};

            std::vector<llvm::Value*> args;
            llvm::Value*              sret_slot{nullptr};
            if (shape.sret_type) {
                sret_slot = aggregate_copier::entry_alloca(caller, shape.sret_type);
                args.emplace_back(sret_slot);
            }
            std::vector<llvm::Value*> copied_from;
            for (u32 idx{0}; idx < call->arg_size(); ++idx) {
                auto* arg{call->getArgOperand(idx)};
                auto* aggregate{idx < shape.indirect_params.size() ? shape.indirect_params[idx]
                                                                   : nullptr};
                if (!aggregate) {
                    args.emplace_back(arg);
                    continue;
                }
                auto* slot{aggregate_copier::entry_alloca(caller, aggregate)};
                copier_.materialize_argument(arg, slot, aggregate, *call);
                args.emplace_back(slot);
                copied_from.emplace_back(arg);
            }

            llvm::IRBuilder<> builder{call};
            auto* lowered{builder.CreateCall(shape.lowered, call->getCalledOperand(), args)};
            lowered->setCallingConv(call->getCallingConv());
            lowered->setAttributes(llvm::AttributeList::get(
                context_, call->getAttributes().getFnAttrs(), llvm::AttributeSet{}, {}));
            if (shape.sret_type) {
                lowered->addParamAttr(
                    0, llvm::Attribute::getWithStructRetType(context_, shape.sret_type));
                copier_.redirect_sret_result(*call, sret_slot, shape.sret_type);
            } else if (!call->getType()->isVoidTy()) {
                lowered->takeName(call);
                call->replaceAllUsesWith(lowered);
            }
            call->eraseFromParent();
            std::ranges::sort(copied_from);
            copied_from.erase(std::ranges::unique(copied_from).begin(), copied_from.end());
            for (auto* arg : copied_from) { aggregate_copier::erase_if_dead(arg); }
        }
    }

    // Any remaining whole-aggregate store of a large value becomes a byte copy
    auto rewrite_copies() -> void {
        PROFILE_FUNCTION();
        std::vector<llvm::StoreInst*> stores;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                auto* store{llvm::dyn_cast<llvm::StoreInst>(&inst)};
                if (store && !store->isVolatile() &&
                    is_large(store->getValueOperand()->getType())) {
                    stores.emplace_back(store);
                }
            }
        }
        for (auto* store : stores) {
            auto* value{store->getValueOperand()};
            if (llvm::isa<llvm::UndefValue>(value)) {
                store->eraseFromParent();
                continue;
            }
            auto* src{copier_.source_address(value, store)};
            if (!src) { continue; }
            llvm::IRBuilder<> builder{store};
            copier_.copy_bytes(builder, store->getPointerOperand(), src, value->getType());
            store->eraseFromParent();
            aggregate_copier::erase_if_dead(value);
        }
    }

  private:
    llvm::Module&           module_;
    const llvm::DataLayout& layout_;
    llvm::LLVMContext&      context_;
    aggregate_copier        copier_;
};

} // namespace

auto lower_large_aggregates(llvm::Module& module) -> bool {
    PROFILE_FUNCTION();
    return aggregate_lowering{module}.run();
}

} // namespace ghoti::codegen
