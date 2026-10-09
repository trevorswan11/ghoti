#include "compiler/codegen/soft_f80.hh"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/Support/Casting.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ValueMapper.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"

namespace ghoti::codegen {

namespace {

constexpr std::array routines{
    // Arithmetic
    "__addxf3",
    "__subxf3",
    "__mulxf3",
    "__divxf3",
    "__fmodx",
    // Comparison
    "__eqxf2",
    "__nexf2",
    "__ltxf2",
    "__lexf2",
    "__gtxf2",
    "__gexf2",
    "__unordxf2",
    // Float conversion
    "__extendhfxf2",
    "__extendsfxf2",
    "__extenddfxf2",
    "__extendxftf2",
    "__truncxfhf2",
    "__truncxfsf2",
    "__truncxfdf2",
    "__trunctfxf2",
    // Integer conversion
    "__floatsixf",
    "__floatdixf",
    "__floattixf",
    "__floateixf",
    "__floatunsixf",
    "__floatundixf",
    "__floatuntixf",
    "__floatuneixf",
    "__fixxfsi",
    "__fixxfdi",
    "__fixxfti",
    "__fixxfei",
    "__fixunsxfsi",
    "__fixunsxfdi",
    "__fixunsxfti",
    "__fixunsxfei",
    // Integers wider than 128 bits to and from the other formats
    "__floateihf",
    "__floateisf",
    "__floateidf",
    "__floateitf",
    "__floatuneihf",
    "__floatuneisf",
    "__floatuneidf",
    "__floatuneitf",
    "__fixhfei",
    "__fixsfei",
    "__fixdfei",
    "__fixtfei",
    "__fixunshfei",
    "__fixunssfei",
    "__fixunsdfei",
    "__fixunstfei",
    // Math
    "__sqrtx",
    "__floorx",
    "__ceilx",
    "__truncx",
    "__roundx",
    "__sinx",
    "__cosx",
    "__tanx",
    "__expx",
    "__exp2x",
    "__logx",
    "__log2x",
    "__log10x",
    "__fmax",
    "__fminx",
    "__fmaxx",
};

// The routine for an `llvm.*` intrinsic over `f80`, if compiler_rt has one
[[nodiscard]] auto intrinsic_routine(llvm::Intrinsic::ID id) -> stdx::option<std::string_view> {
    switch (id) {
    case llvm::Intrinsic::sqrt:    return "__sqrtx";
    case llvm::Intrinsic::floor:   return "__floorx";
    case llvm::Intrinsic::ceil:    return "__ceilx";
    case llvm::Intrinsic::trunc:   return "__truncx";
    case llvm::Intrinsic::round:   return "__roundx";
    case llvm::Intrinsic::sin:     return "__sinx";
    case llvm::Intrinsic::cos:     return "__cosx";
    case llvm::Intrinsic::tan:     return "__tanx";
    case llvm::Intrinsic::exp:     return "__expx";
    case llvm::Intrinsic::exp2:    return "__exp2x";
    case llvm::Intrinsic::log:     return "__logx";
    case llvm::Intrinsic::log2:    return "__log2x";
    case llvm::Intrinsic::log10:   return "__log10x";
    case llvm::Intrinsic::fma:
    case llvm::Intrinsic::fmuladd: return "__fmax";
    case llvm::Intrinsic::minnum:  return "__fminx";
    case llvm::Intrinsic::maxnum:  return "__fmaxx";
    default:                       return stdx::none;
    }
}

// compiler_rt's abbreviation for a float type in a routine name
[[nodiscard]] auto float_abbrev(const llvm::Type* type) -> std::string_view {
    if (type->isHalfTy()) { return "hf"; }
    if (type->isFloatTy()) { return "sf"; }
    if (type->isDoubleTy()) { return "df"; }
    if (type->isFP128Ty()) { return "tf"; }
    return "";
}

// compiler_rt's abbreviation for the integer width a conversion goes through
[[nodiscard]] auto int_abbrev(u32 bits) -> std::string_view {
    if (bits <= 32) { return "si"; }
    if (bits <= 64) { return "di"; }
    if (bits <= 128) { return "ti"; }
    return "ei";
}

// Stage one: every `f80` operation becomes a call, or integer math on its bits. So does every
// conversion between a float and an integer wider than 128 bits, which LLVM expands only on x86
class operation_softener {
  public:
    explicit operation_softener(llvm::Module& module)
        : module_{module}, builder_{module.getContext()},
          f80_{llvm::Type::getX86_FP80Ty(module.getContext())},
          i32_{llvm::Type::getInt32Ty(module.getContext())},
          sign_mask_{llvm::APInt::getSignMask(80)} {}

    auto run() -> void {
        for (auto& fn : module_) {
            std::vector<llvm::Instruction*> targets;
            for (auto& inst : llvm::instructions(fn)) {
                if (needs_rewrite(inst)) { targets.emplace_back(&inst); }
            }
            for (auto* inst : targets) {
                builder_.SetInsertPoint(inst);
                if (auto* replacement{rewrite(*inst)}) {
                    replacement->takeName(inst);
                    inst->replaceAllUsesWith(replacement);
                    inst->eraseFromParent();
                }
            }
        }
        // The `llvm.*.f80` declarations the rewritten calls leave behind
        std::vector<llvm::Function*> unused;
        for (auto& fn : module_) {
            if (fn.isIntrinsic() && fn.use_empty() && is_f80(fn.getReturnType())) {
                unused.emplace_back(&fn);
            }
        }
        for (auto* fn : unused) { fn->eraseFromParent(); }
    }

  private:
    [[nodiscard]] auto is_f80(const llvm::Type* type) const -> bool { return type == f80_; }

    [[nodiscard]] static auto is_wide_int(const llvm::Type* type) -> bool {
        return type->isIntegerTy() && type->getIntegerBitWidth() > 128;
    }

    // The format part of a conversion routine's name
    [[nodiscard]] auto format_abbrev(const llvm::Type* type) const -> std::string_view {
        return is_f80(type) ? "xf" : float_abbrev(type);
    }

    [[nodiscard]] auto needs_rewrite(const llvm::Instruction& inst) const -> bool {
        switch (inst.getOpcode()) {
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
        case llvm::Instruction::FNeg:
        case llvm::Instruction::FCmp: return is_f80(inst.getOperand(0)->getType());
        case llvm::Instruction::FPToSI:
        case llvm::Instruction::FPToUI:
            return is_f80(inst.getOperand(0)->getType()) || is_wide_int(inst.getType());
        case llvm::Instruction::SIToFP:
        case llvm::Instruction::UIToFP:
            return is_f80(inst.getType()) || is_wide_int(inst.getOperand(0)->getType());
        case llvm::Instruction::FPExt:
        case llvm::Instruction::FPTrunc:
            return is_f80(inst.getType()) || is_f80(inst.getOperand(0)->getType());
        case llvm::Instruction::Call:
            if (const auto* intrinsic{llvm::dyn_cast<llvm::IntrinsicInst>(&inst)}) {
                const auto id{intrinsic->getIntrinsicID()};
                if (!is_f80(inst.getType())) { return false; }
                return id == llvm::Intrinsic::fabs || id == llvm::Intrinsic::copysign ||
                       intrinsic_routine(id).has_value();
            }
            return false;
        default: return false;
        }
    }

    [[nodiscard]] auto rewrite(llvm::Instruction& inst) -> llvm::Value* {
        switch (inst.getOpcode()) {
        case llvm::Instruction::FAdd: return binary("__addxf3", inst);
        case llvm::Instruction::FSub: return binary("__subxf3", inst);
        case llvm::Instruction::FMul: return binary("__mulxf3", inst);
        case llvm::Instruction::FDiv: return binary("__divxf3", inst);
        case llvm::Instruction::FRem: return binary("__fmodx", inst);
        case llvm::Instruction::FNeg: return flip_sign(inst.getOperand(0));
        case llvm::Instruction::FCmp: return compare(llvm::cast<llvm::FCmpInst>(inst));
        case llvm::Instruction::FPExt:
        case llvm::Instruction::FPTrunc:
            return float_conversion(inst.getOperand(0), inst.getType());
        case llvm::Instruction::SIToFP: return from_int(inst.getOperand(0), inst.getType(), true);
        case llvm::Instruction::UIToFP: return from_int(inst.getOperand(0), inst.getType(), false);
        case llvm::Instruction::FPToSI: return to_int(inst.getOperand(0), inst.getType(), true);
        case llvm::Instruction::FPToUI: return to_int(inst.getOperand(0), inst.getType(), false);
        case llvm::Instruction::Call:   return intrinsic(llvm::cast<llvm::IntrinsicInst>(inst));
        default:                        return nullptr;
        }
    }

    // A call to a routine that only computes on its operands
    auto call(std::string_view name, llvm::Type* result, llvm::ArrayRef<llvm::Value*> args)
        -> llvm::CallInst* {
        std::vector<llvm::Type*> params;
        params.reserve(args.size());
        for (auto* arg : args) { params.emplace_back(arg->getType()); }
        auto callee{
            module_.getOrInsertFunction(name, llvm::FunctionType::get(result, params, false))};
        auto* call{builder_.CreateCall(callee, args)};
        call->setDoesNotThrow();
        if (std::ranges::none_of(args, [](auto* arg) { return arg->getType()->isPointerTy(); })) {
            call->setDoesNotAccessMemory();
        }
        return call;
    }

    auto binary(std::string_view name, llvm::Instruction& inst) -> llvm::Value* {
        return call(name, f80_, {inst.getOperand(0), inst.getOperand(1)});
    }

    auto flip_sign(llvm::Value* value) -> llvm::Value* {
        auto* i80{builder_.getIntNTy(80)};
        auto* flipped{builder_.CreateXor(builder_.CreateBitCast(value, i80),
                                         llvm::ConstantInt::get(i80, sign_mask_))};
        return builder_.CreateBitCast(flipped, f80_);
    }

    // libgcc's comparison convention, as LLVM softens every other float type:
    // `__lt`/`__le` answer > 0 and `__gt`/`__ge` answer < 0 for unordered operands
    auto compare(llvm::FCmpInst& cmp) -> llvm::Value* {
        auto*      lhs{cmp.getOperand(0)};
        auto*      rhs{cmp.getOperand(1)};
        auto*      zero{llvm::ConstantInt::get(i32_, 0)};
        const auto test{[&](std::string_view routine, llvm::CmpInst::Predicate pred) {
            return builder_.CreateICmp(pred, call(routine, i32_, {lhs, rhs}), zero);
        }};
        using P = llvm::CmpInst::Predicate;
        switch (cmp.getPredicate()) {
        case P::FCMP_FALSE: return builder_.getFalse();
        case P::FCMP_TRUE:  return builder_.getTrue();
        case P::FCMP_OEQ:   return test("__eqxf2", P::ICMP_EQ);
        case P::FCMP_UNE:   return test("__nexf2", P::ICMP_NE);
        case P::FCMP_OLT:   return test("__ltxf2", P::ICMP_SLT);
        case P::FCMP_OLE:   return test("__lexf2", P::ICMP_SLE);
        case P::FCMP_OGT:   return test("__gtxf2", P::ICMP_SGT);
        case P::FCMP_OGE:   return test("__gexf2", P::ICMP_SGE);
        case P::FCMP_UNO:   return test("__unordxf2", P::ICMP_NE);
        case P::FCMP_ORD:   return test("__unordxf2", P::ICMP_EQ);
        case P::FCMP_ULT:   return test("__gexf2", P::ICMP_SLT);
        case P::FCMP_ULE:   return test("__gtxf2", P::ICMP_SLE);
        case P::FCMP_UGT:   return test("__lexf2", P::ICMP_SGT);
        case P::FCMP_UGE:   return test("__ltxf2", P::ICMP_SGE);
        case P::FCMP_ONE:
            return builder_.CreateOr(test("__ltxf2", P::ICMP_SLT), test("__gtxf2", P::ICMP_SGT));
        case P::FCMP_UEQ:
            return builder_.CreateOr(test("__unordxf2", P::ICMP_NE), test("__eqxf2", P::ICMP_EQ));
        default: return nullptr;
        }
    }

    auto float_conversion(llvm::Value* value, llvm::Type* target) -> llvm::Value* {
        const bool widening{is_f80(target)};
        const auto other{float_abbrev(widening ? value->getType() : target)};
        if (other.empty()) { return nullptr; } // `bfloat`, which ghoti never lowers to
        // `f128` is the one format wider than `f80`
        const auto name{
            other == "tf" ? (widening ? std::string{"__trunctfxf2"} : std::string{"__extendxftf2"})
            : widening    ? fmt::format("__extend{}xf2", other)
                          : fmt::format("__truncxf{}2", other)};
        return call(name, target, {value});
    }

    // `__float*xf` takes the narrowest of i32/i64/i128 holding the operand, or reads a wider
    // integer through memory
    auto from_int(llvm::Value* value, llvm::Type* target, bool is_signed) -> llvm::Value* {
        const auto bits{value->getType()->getIntegerBitWidth()};
        const auto abbrev{int_abbrev(bits)};
        const auto format{format_abbrev(target)};
        if (format.empty()) { return nullptr; }
        const auto name{fmt::format("__float{}{}{}", is_signed ? "" : "un", abbrev, format)};
        if (bits > 128) {
            auto* slot{spill(value)};
            return call(name, target, {slot, bit_count(bits)});
        }
        auto* wide{builder_.getIntNTy(bits <= 32 ? 32 : bits <= 64 ? 64 : 128)};
        auto* extended{is_signed ? builder_.CreateSExt(value, wide)
                                 : builder_.CreateZExt(value, wide)};
        return call(name, target, {extended});
    }

    auto to_int(llvm::Value* value, llvm::Type* target, bool is_signed) -> llvm::Value* {
        const auto bits{target->getIntegerBitWidth()};
        const auto format{format_abbrev(value->getType())};
        if (format.empty()) { return nullptr; }
        const auto name{
            fmt::format("__fix{}{}{}", is_signed ? "" : "uns", format, int_abbrev(bits))};
        if (bits > 128) {
            auto* slot{entry_alloca(target)};
            auto* write{call(name, builder_.getVoidTy(), {slot, bit_count(bits), value})};
            write->setOnlyWritesMemory();
            return builder_.CreateLoad(target, slot);
        }
        auto* wide{builder_.getIntNTy(bits <= 32 ? 32 : bits <= 64 ? 64 : 128)};
        return builder_.CreateTrunc(call(name, wide, {value}), target);
    }

    auto intrinsic(llvm::IntrinsicInst& inst) -> llvm::Value* {
        const auto id{inst.getIntrinsicID()};
        // Sign-bit operations never need a routine
        if (id == llvm::Intrinsic::fabs || id == llvm::Intrinsic::copysign) {
            auto* i80{builder_.getIntNTy(80)};
            auto* magnitude{builder_.CreateAnd(builder_.CreateBitCast(inst.getArgOperand(0), i80),
                                               llvm::ConstantInt::get(i80, ~sign_mask_))};
            if (id == llvm::Intrinsic::copysign) {
                auto* sign{builder_.CreateAnd(builder_.CreateBitCast(inst.getArgOperand(1), i80),
                                              llvm::ConstantInt::get(i80, sign_mask_))};
                magnitude = builder_.CreateOr(magnitude, sign);
            }
            return builder_.CreateBitCast(magnitude, f80_);
        }
        std::vector<llvm::Value*> args{inst.arg_begin(), inst.arg_end()};
        return call(*intrinsic_routine(id), f80_, args);
    }

    // Integer conversion routines wider than 128 bits pass the bit count as a `usize`
    [[nodiscard]] auto bit_count(u32 bits) -> llvm::Value* {
        return llvm::ConstantInt::get(module_.getDataLayout().getIntPtrType(module_.getContext()),
                                      bits);
    }

    auto entry_alloca(llvm::Type* type) -> llvm::AllocaInst* {
        auto*      fn{builder_.GetInsertBlock()->getParent()};
        const auto saved{builder_.saveIP()};
        builder_.SetInsertPoint(fn->getEntryBlock().getFirstInsertionPt());
        auto* slot{builder_.CreateAlloca(type)};
        builder_.restoreIP(saved);
        return slot;
    }

    auto spill(llvm::Value* value) -> llvm::AllocaInst* {
        auto* slot{entry_alloca(value->getType())};
        builder_.CreateStore(value, slot);
        return slot;
    }

  private:
    llvm::Module&     module_;
    llvm::IRBuilder<> builder_;
    llvm::Type*       f80_;
    llvm::Type*       i32_;
    llvm::APInt       sign_mask_;
};

// Stage two: `x86_fp80` becomes `i80` through every type holding one, as LLVM's own `f80`
// softening miscompiles loads, stores, and constants on most targets without x87
class storage_mapper final : public llvm::ValueMapTypeRemapper {
  public:
    explicit storage_mapper(llvm::LLVMContext& context)
        : context_{context}, f80_{llvm::Type::getX86_FP80Ty(context)},
          i80_{llvm::Type::getIntNTy(context, 80)} {}

    auto remapType(llvm::Type* type) -> llvm::Type* override {
        if (const auto it{mapped_.find(type)}; it != mapped_.end()) { return it->second; }
        auto* mapped{holds_f80(type) ? map(type) : type};
        mapped_[type] = mapped;
        return mapped;
    }

    // Each aggregate type replaced, so its layout can be checked against the original
    [[nodiscard]] auto replaced_structs() const
        -> const std::vector<std::pair<llvm::StructType*, llvm::StructType*>>& {
        return structs_;
    }

  private:
    [[nodiscard]] auto holds_f80(llvm::Type* type) const -> bool {
        if (type == f80_) { return true; }
        if (const auto* st{llvm::dyn_cast<llvm::StructType>(type)}; st && st->isOpaque()) {
            return false;
        }
        return std::ranges::any_of(type->subtypes(),
                                   [this](llvm::Type* inner) { return holds_f80(inner); });
    }

    auto map(llvm::Type* type) -> llvm::Type* {
        if (type == f80_) { return i80_; }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(type)}) { return map_struct(*st); }
        if (auto* at{llvm::dyn_cast<llvm::ArrayType>(type)}) {
            return llvm::ArrayType::get(remapType(at->getElementType()), at->getNumElements());
        }
        if (auto* vt{llvm::dyn_cast<llvm::FixedVectorType>(type)}) {
            return llvm::FixedVectorType::get(remapType(vt->getElementType()),
                                              vt->getNumElements());
        }
        if (auto* ft{llvm::dyn_cast<llvm::FunctionType>(type)}) {
            std::vector<llvm::Type*> params;
            for (auto* param : ft->params()) { params.emplace_back(remapType(param)); }
            return llvm::FunctionType::get(remapType(ft->getReturnType()), params, ft->isVarArg());
        }
        return type;
    }

    auto map_struct(llvm::StructType& st) -> llvm::StructType* {
        llvm::StructType* replacement{nullptr};
        if (!st.isLiteral()) {
            // Registered before its body, which may name it again
            replacement  = llvm::StructType::create(context_);
            mapped_[&st] = replacement;
            const auto name{st.getName().str()};
            st.setName("");
            replacement->setName(name);
        }
        std::vector<llvm::Type*> elements;
        for (auto* element : st.elements()) { elements.emplace_back(remapType(element)); }
        if (replacement) {
            replacement->setBody(elements, st.isPacked());
        } else {
            replacement = llvm::StructType::get(context_, elements, st.isPacked());
        }
        structs_.emplace_back(&st, replacement);
        return replacement;
    }

  private:
    llvm::LLVMContext&                                           context_;
    llvm::Type*                                                  f80_;
    llvm::Type*                                                  i80_;
    ankerl::unordered_dense::map<llvm::Type*, llvm::Type*>       mapped_;
    std::vector<std::pair<llvm::StructType*, llvm::StructType*>> structs_;
};

// Type-carrying attributes (`sret(T)`, `byval(T)`, ...) name the new types too
[[nodiscard]] auto remap_attributes(llvm::AttributeList attrs,
                                    llvm::LLVMContext&  context,
                                    storage_mapper&     mapper) -> llvm::AttributeList {
    for (u32 i{0}; i < attrs.getNumAttrSets(); ++i) {
        for (auto kind{static_cast<i32>(llvm::Attribute::FirstTypeAttr)};
             kind <= static_cast<i32>(llvm::Attribute::LastTypeAttr);
             ++kind) {
            const auto attr_kind{static_cast<llvm::Attribute::AttrKind>(kind)};
            if (auto* type{attrs.getAttributeAtIndex(i, attr_kind).getValueAsType()}) {
                attrs = attrs.replaceAttributeTypeAtIndex(
                    context, i, attr_kind, mapper.remapType(type));
            }
        }
    }
    return attrs;
}

class storage_rewriter {
  public:
    explicit storage_rewriter(llvm::Module& module)
        : module_{module}, context_{module.getContext()}, mapper_{module.getContext()},
          f80_{llvm::Type::getX86_FP80Ty(module.getContext())} {}

    auto run() -> stdx::result<void, diagnostic> {
        TRY(check_scalar_size());
        seed_constants();
        replace_functions();
        replace_globals();
        for (auto& fn : module_) {
            if (!fn.isDeclaration()) { llvm::RemapFunction(fn, values_, flags, &mapper_); }
        }
        // Only now that no body still names their arguments
        for (auto* fn : replaced_functions_) { fn->eraseFromParent(); }
        return check_layouts();
    }

  private:
    static constexpr auto flags{
        static_cast<llvm::RemapFlags>(std::to_underlying(llvm::RF_IgnoreMissingLocals) |
                                      std::to_underlying(llvm::RF_NoModuleLevelChanges))};

  private:
    // An `f80` array's stride must not move
    [[nodiscard]] auto check_scalar_size() const -> stdx::result<void, diagnostic> {
        const auto& dl{module_.getDataLayout()};
        if (dl.getTypeAllocSize(f80_) == dl.getTypeAllocSize(llvm::Type::getIntNTy(context_, 80))) {
            return {};
        }
        return layout_error();
    }

    // Nor may any offset the lowering computed against the `f80` layout
    [[nodiscard]] auto check_layouts() const -> stdx::result<void, diagnostic> {
        const auto& dl{module_.getDataLayout()};
        for (const auto& [before, after] : mapper_.replaced_structs()) {
            if (!before->isSized()) { continue; }
            const auto* old_layout{dl.getStructLayout(before)};
            const auto* new_layout{dl.getStructLayout(after)};
            bool        same{old_layout->getSizeInBytes() == new_layout->getSizeInBytes()};
            for (u32 i{0}; same && i < before->getNumElements(); ++i) {
                same = old_layout->getElementOffset(i) == new_layout->getElementOffset(i);
            }
            if (!same) { return layout_error(); }
        }
        return {};
    }

    [[nodiscard]] auto layout_error() const -> stdx::err<diagnostic> {
        return make_codegen_err(
            fmt::format("an aggregate holding an 'f80' can't keep its layout on '{}', where an "
                        "'f80' is stored as an 80-bit integer",
                        module_.getTargetTriple().str()),
            error::UNSUPPORTED_TARGET);
    }

    // `ValueMapper` can't retype a float constant on its own, so map each to its bits up front
    auto seed_constants() -> void {
        const auto seed{[this](this const auto& self, llvm::Constant* constant) -> void {
            if (auto* fp{llvm::dyn_cast<llvm::ConstantFP>(constant)}) {
                if (fp->getType() == f80_) {
                    values_[fp] =
                        llvm::ConstantInt::get(context_, fp->getValueAPF().bitcastToAPInt());
                }
                return;
            }
            if (llvm::isa<llvm::GlobalValue>(constant)) { return; }
            for (auto& operand : constant->operands()) {
                if (auto* inner{llvm::dyn_cast<llvm::Constant>(operand)}) { self(inner); }
            }
        }};
        for (auto& global : module_.globals()) {
            if (global.hasInitializer()) { seed(global.getInitializer()); }
        }
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                for (auto& operand : inst.operands()) {
                    if (auto* constant{llvm::dyn_cast<llvm::Constant>(operand)}) { seed(constant); }
                }
            }
        }
    }

    // A function's signature can't change in place, so a retyped one takes over its body
    auto replace_functions() -> void {
        std::vector<llvm::Function*> stale;
        for (auto& fn : module_) {
            if (fn.isIntrinsic()) { continue; }
            if (mapper_.remapType(fn.getFunctionType()) != fn.getFunctionType()) {
                stale.emplace_back(&fn);
            }
        }
        for (auto* fn : stale) {
            auto* type{llvm::cast<llvm::FunctionType>(mapper_.remapType(fn->getFunctionType()))};
            auto* replacement{
                llvm::Function::Create(type, fn->getLinkage(), fn->getAddressSpace())};
            module_.getFunctionList().insert(fn->getIterator(), replacement);
            replacement->copyAttributesFrom(fn);
            replacement->setAttributes(remap_attributes(fn->getAttributes(), context_, mapper_));
            replacement->setComdat(fn->getComdat());
            replacement->copyMetadata(fn, 0);
            replacement->splice(replacement->begin(), fn);
            for (auto [old_arg, new_arg] : llvm::zip(fn->args(), replacement->args())) {
                new_arg.takeName(&old_arg);
                values_[&old_arg] = &new_arg;
            }
            replacement->takeName(fn);
            fn->replaceAllUsesWith(replacement);
            replaced_functions_.emplace_back(fn);
        }
    }

    auto replace_globals() -> void {
        const auto&                        dl{module_.getDataLayout()};
        std::vector<llvm::GlobalVariable*> stale;
        for (auto& global : module_.globals()) {
            if (mapper_.remapType(global.getValueType()) != global.getValueType()) {
                stale.emplace_back(&global);
            }
        }
        std::vector<std::pair<llvm::GlobalVariable*, llvm::GlobalVariable*>> replaced;
        for (auto* global : stale) {
            auto* replacement{new llvm::GlobalVariable{module_,
                                                       mapper_.remapType(global->getValueType()),
                                                       global->isConstant(),
                                                       global->getLinkage(),
                                                       nullptr,
                                                       "",
                                                       global,
                                                       global->getThreadLocalMode(),
                                                       global->getAddressSpace(),
                                                       global->isExternallyInitialized()}};
            replacement->copyAttributesFrom(global);
            // An `i80` may align less than the `f80` the global was laid out for
            if (!global->getAlign()) {
                replacement->setAlignment(dl.getPrefTypeAlign(global->getValueType()));
            }
            replacement->setComdat(global->getComdat());
            replacement->copyMetadata(global, 0);
            replacement->takeName(global);
            global->replaceAllUsesWith(replacement);
            replaced.emplace_back(global, replacement);
        }
        // Initializers last, once every global they point at has its replacement
        for (const auto& [global, replacement] : replaced) {
            if (global->hasInitializer()) {
                replacement->setInitializer(
                    llvm::MapValue(global->getInitializer(), values_, flags, &mapper_));
            }
            global->eraseFromParent();
        }
    }

  private:
    llvm::Module&                module_;
    llvm::LLVMContext&           context_;
    storage_mapper               mapper_;
    llvm::ValueToValueMapTy      values_;
    llvm::Type*                  f80_;
    std::vector<llvm::Function*> replaced_functions_;
};

[[nodiscard]] auto storage_align(const llvm::DataLayout& dl, llvm::Type* type) -> u64 {
    if (type->isX86_FP80Ty()) {
        return dl.getABITypeAlign(llvm::Type::getIntNTy(type->getContext(), 80)).value();
    }
    if (auto* st{llvm::dyn_cast<llvm::StructType>(type)}; st && !st->isOpaque()) {
        if (st->isPacked()) { return 1; }
        u64 align{1};
        for (auto* element : st->elements()) {
            align = std::max(align, storage_align(dl, element));
        }
        return align;
    }
    if (auto* at{llvm::dyn_cast<llvm::ArrayType>(type)}) {
        return storage_align(dl, at->getElementType());
    }
    return dl.getABITypeAlign(type).value();
}

} // namespace

auto needs_soft_f80(const llvm::Triple& triple) -> bool {
    return triple.getArch() != llvm::Triple::UnknownArch && !triple.isX86();
}

auto soften_f80(llvm::Module& module) -> stdx::result<void, diagnostic> {
    if (!needs_soft_f80(module.getTargetTriple())) { return {}; }
    operation_softener{module}.run();
    return storage_rewriter{module}.run();
}

auto f80_storage_align(const llvm::Module& module, llvm::Type* type) -> u64 {
    const auto& dl{module.getDataLayout()};
    if (!needs_soft_f80(module.getTargetTriple())) { return dl.getABITypeAlign(type).value(); }
    return storage_align(dl, type);
}

auto is_soft_f80_routine(std::string_view symbol) -> bool {
    return std::ranges::contains(routines, symbol);
}

} // namespace ghoti::codegen
