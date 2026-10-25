#include "compiler/codegen/wide_int_arith.hh"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/Casting.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::codegen {

namespace {

enum class routine : u8 {
    MUL,
    UMULO,
    SMULO,
    UDIV,
    UMOD,
    SDIV,
    SMOD,
};

constexpr std::array<std::string_view, 7> routine_names{
    "__ghoti_mulei",
    "__ghoti_umuloei",
    "__ghoti_smuloei",
    "__ghoti_udivei",
    "__ghoti_umodei",
    "__ghoti_sdivei",
    "__ghoti_smodei",
};

constexpr u32 LIMB_BITS{32};

// Vector lanes are at most 128 bits, so only a scalar is ever this wide
[[nodiscard]] auto is_wide(const llvm::Type* type) -> bool {
    return type->isIntegerTy() && type->getIntegerBitWidth() > MAX_INLINE_MULDIV_BITS;
}

[[nodiscard]] auto reports_overflow(routine r) -> bool {
    return r == routine::UMULO || r == routine::SMULO;
}

[[nodiscard]] auto is_signed(routine r) -> bool {
    return r == routine::SMULO || r == routine::SDIV || r == routine::SMOD;
}

[[nodiscard]] auto routine_for(const llvm::Instruction& inst) -> stdx::option<routine> {
    switch (inst.getOpcode()) {
    case llvm::Instruction::Mul:  return routine::MUL;
    case llvm::Instruction::UDiv: return routine::UDIV;
    case llvm::Instruction::URem: return routine::UMOD;
    case llvm::Instruction::SDiv: return routine::SDIV;
    case llvm::Instruction::SRem: return routine::SMOD;
    default:                      break;
    }
    const auto* intrinsic{llvm::dyn_cast<llvm::IntrinsicInst>(&inst)};
    if (!intrinsic) { return stdx::none; }
    switch (intrinsic->getIntrinsicID()) {
    case llvm::Intrinsic::umul_with_overflow:
    case llvm::Intrinsic::umul_fix_sat:       return routine::UMULO;
    case llvm::Intrinsic::smul_with_overflow:
    case llvm::Intrinsic::smul_fix_sat:       return routine::SMULO;
    default:                                  return stdx::none;
    }
}

// Only the scale-0 fixed-point multiply is a plain saturating one
[[nodiscard]] auto is_plain_saturating_mul(const llvm::IntrinsicInst& inst) -> bool {
    const auto* scale{llvm::dyn_cast<llvm::ConstantInt>(inst.getArgOperand(2))};
    return scale && scale->isZero();
}

class expander {
  public:
    explicit expander(llvm::Module& module)
        : module_{module}, builder_{module.getContext()},
          ptr_ty_{llvm::PointerType::getUnqual(module.getContext())},
          size_ty_{module.getDataLayout().getIntPtrType(module.getContext())} {}

    auto run() -> void {
        std::vector<std::pair<llvm::Instruction*, routine>> work;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                const auto r{routine_for(inst)};
                if (!r || !is_wide(inst.getOperand(0)->getType())) { continue; }
                const auto* intrinsic{llvm::dyn_cast<llvm::IntrinsicInst>(&inst)};
                const bool  saturating{
                    intrinsic && (intrinsic->getIntrinsicID() == llvm::Intrinsic::umul_fix_sat ||
                                  intrinsic->getIntrinsicID() == llvm::Intrinsic::smul_fix_sat)};
                if (saturating && !is_plain_saturating_mul(*intrinsic)) { continue; }
                work.emplace_back(&inst, *r);
            }
        }
        for (const auto& [inst, r] : work) {
            builder_.SetInsertPoint(inst);
            auto* replacement{expand(*inst, r)};
            replacement->takeName(inst);
            inst->replaceAllUsesWith(replacement);
            inst->eraseFromParent();
        }
    }

  private:
    using slots_t = std::array<llvm::AllocaInst*, 3>;

  private:
    [[nodiscard]] auto expand(llvm::Instruction& inst, routine r) -> llvm::Value* {
        auto*       a{inst.getOperand(0)};
        auto*       b{inst.getOperand(1)};
        const auto* intrinsic{llvm::dyn_cast<llvm::IntrinsicInst>(&inst)};
        if (!intrinsic) { return call(a, b, r).first; }

        auto [product, overflow]{call(a, b, r)};
        if (intrinsic->getIntrinsicID() == llvm::Intrinsic::umul_with_overflow ||
            intrinsic->getIntrinsicID() == llvm::Intrinsic::smul_with_overflow) {
            auto* pair{
                builder_.CreateInsertValue(llvm::PoisonValue::get(inst.getType()), product, {0U})};
            return builder_.CreateInsertValue(pair, overflow, {1U});
        }
        return builder_.CreateSelect(overflow, saturation(a, b, r), product);
    }

    // The bound an overflowing saturating multiply clamps to: the sign of the true product picks
    // the signed one
    [[nodiscard]] auto saturation(llvm::Value* a, llvm::Value* b, routine r) -> llvm::Value* {
        auto*      type{a->getType()};
        const auto width{type->getIntegerBitWidth()};
        if (!is_signed(r)) { return llvm::ConstantInt::get(type, llvm::APInt::getMaxValue(width)); }
        auto* zero{llvm::ConstantInt::get(type, 0)};
        auto* negative{
            builder_.CreateXor(builder_.CreateICmpSLT(a, zero), builder_.CreateICmpSLT(b, zero))};
        return builder_.CreateSelect(
            negative,
            llvm::ConstantInt::get(type, llvm::APInt::getSignedMinValue(width)),
            llvm::ConstantInt::get(type, llvm::APInt::getSignedMaxValue(width)));
    }

    // Spills both operands, extended through their top limb, and has the routine write the result
    [[nodiscard]] auto call(llvm::Value* a, llvm::Value* b, routine r)
        -> std::pair<llvm::Value*, llvm::Value*> {
        auto*      type{a->getType()};
        const auto width{type->getIntegerBitWidth()};
        const auto limbs{(width + LIMB_BITS - 1) / LIMB_BITS};
        auto*      padded_ty{builder_.getIntNTy(limbs * LIMB_BITS)};
        const auto extend{[&](llvm::Value* v) {
            return is_signed(r) ? builder_.CreateSExt(v, padded_ty)
                                : builder_.CreateZExt(v, padded_ty);
        }};

        const auto& slots{slots_for(padded_ty)};
        builder_.CreateStore(extend(a), slots[1]);
        builder_.CreateStore(extend(b), slots[2]);
        auto* call{builder_.CreateCall(
            declaration(r),
            {slots[0], slots[1], slots[2], llvm::ConstantInt::get(size_ty_, width)})};
        call->setDoesNotThrow();
        if (reports_overflow(r)) { call->addRetAttr(llvm::Attribute::ZExt); }

        auto* result{builder_.CreateTrunc(builder_.CreateLoad(padded_ty, slots[0]), type)};
        return {result, reports_overflow(r) ? call : nullptr};
    }

    [[nodiscard]] auto declaration(routine r) -> llvm::FunctionCallee {
        auto* ret_ty{reports_overflow(r) ? builder_.getInt1Ty() : builder_.getVoidTy()};
        auto* fn_ty{llvm::FunctionType::get(ret_ty, {ptr_ty_, ptr_ty_, ptr_ty_, size_ty_}, false)};
        auto  callee{module_.getOrInsertFunction(routine_names[static_cast<usize>(r)], fn_ty)};
        if (auto* fn{llvm::dyn_cast<llvm::Function>(callee.getCallee())}) {
            fn->setDoesNotThrow();
            if (reports_overflow(r)) { fn->addRetAttr(llvm::Attribute::ZExt); }
        }
        return callee;
    }

    // The result and operand slots for one width, shared by every call in a function: each call
    // stores, calls, and loads before the next one begins
    [[nodiscard]] auto slots_for(llvm::Type* padded_ty) -> const slots_t& {
        auto*      fn{builder_.GetInsertBlock()->getParent()};
        const auto key{std::pair{fn, padded_ty}};
        if (const auto it{slots_.find(key)}; it != slots_.end()) { return it->second; }

        const auto saved{builder_.saveIP()};
        builder_.SetInsertPoint(fn->getEntryBlock().getFirstInsertionPt());
        const std::array slots{builder_.CreateAlloca(padded_ty),
                               builder_.CreateAlloca(padded_ty),
                               builder_.CreateAlloca(padded_ty)};
        builder_.restoreIP(saved);
        return slots_.emplace(key, slots).first->second;
    }

  private:
    llvm::Module&                                                                  module_;
    llvm::IRBuilder<>                                                              builder_;
    llvm::Type*                                                                    ptr_ty_;
    llvm::Type*                                                                    size_ty_;
    ankerl::unordered_dense::map<std::pair<llvm::Function*, llvm::Type*>, slots_t> slots_;
};

} // namespace

auto expand_wide_int_arith(llvm::Module& module) -> void { expander{module}.run(); }

auto is_wide_int_routine(std::string_view symbol) -> bool {
    return std::ranges::contains(routine_names, symbol);
}

} // namespace ghoti::codegen
