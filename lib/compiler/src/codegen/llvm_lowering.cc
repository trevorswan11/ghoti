#include "compiler/codegen/llvm_lowering.hh"

#include <string>
#include <string_view>
#include <utility>

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::codegen {

namespace {

[[nodiscard]] auto is_float_type(const gir::instruction&    inst,
                                 stdx::option<llvm::Value&> val) noexcept -> bool {
    if (!inst.operands.empty() && inst.operands[0].type) {
        return sema::is_float(inst.operands[0].type->get_kind());
    }
    if (inst.type) { return sema::is_float(inst.type->get_kind()); }
    return val && val->getType()->isFloatingPointTy();
}

[[nodiscard]] auto is_signed_type(const gir::instruction& inst) noexcept -> bool {
    if (!inst.operands.empty() && inst.operands[0].type) {
        return sema::is_signed_integer(inst.operands[0].type->get_kind());
    }
    if (inst.type) { return sema::is_signed_integer(inst.type->get_kind()); }
    return false;
}

} // namespace

llvm_lowering::llvm_lowering(llvm::LLVMContext& context, std::string_view module_name) noexcept
    : context_{context}, llvm_module_{stdx::make_box<llvm::Module>(module_name, context_)},
      builder_{context_}, types_{context_} {}

auto llvm_lowering::lower_value(const gir::value& val, const sema::type* expected_type)
    -> llvm::Value* {
    return val.data.visit(
        [this](gir::local_id loc) -> llvm::Value* {
            const auto it{locals_.find(loc)};
            ASSERT(it != locals_.end(), "Local ID not found in codegen map");
            return it->second;
        },
        [this, &val, expected_type](i64 i) -> llvm::Value* {
            auto* ty{val.type        ? types_.translate(*val.type)
                     : expected_type ? types_.translate(*expected_type)
                                     : types_.get_int64_ty()};
            return llvm::ConstantInt::get(ty, static_cast<u64>(i), true);
        },
        [this, &val, expected_type](u64 u) -> llvm::Value* {
            auto* ty{val.type        ? types_.translate(*val.type)
                     : expected_type ? types_.translate(*expected_type)
                                     : types_.get_int64_ty()};
            return llvm::ConstantInt::get(ty, u, false);
        },
        [this, &val, expected_type](f64 f) -> llvm::Value* {
            auto* ty{val.type        ? types_.translate(*val.type)
                     : expected_type ? types_.translate(*expected_type)
                                     : types_.get_double_ty()};
            return llvm::ConstantFP::get(ty, f);
        },
        [this](bool b) -> llvm::Value* { return llvm::ConstantInt::getBool(context_, b); },
        [this](const std::string& str) -> llvm::Value* {
            return builder_.CreateGlobalString(str, "str");
        },
        [this, &val, expected_type](gir::undefined_val) -> llvm::Value* {
            auto* ty{val.type        ? types_.translate(*val.type)
                     : expected_type ? types_.translate(*expected_type)
                                     : types_.get_int64_ty()};
            return llvm::UndefValue::get(ty);
        },
        [](gir::void_val) -> llvm::Value* { return nullptr; },
        [](const stdx::option<sema::type&>&) -> llvm::Value* { return nullptr; });
}

auto llvm_lowering::emit_binary(const gir::instruction& inst) -> llvm::Value* {
    ASSERT(inst.operands.size() >= 2, "Binary instruction requires at least 2 operands");
    auto* lhs{lower_value(inst.operands[0])};
    auto* rhs{lower_value(inst.operands[1])};
    ASSERT(lhs && rhs, "Binary operands must lower to non-null LLVM values");

    const bool is_flt{is_float_type(inst, lhs)};
    const bool is_sgn{is_signed_type(inst)};

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
    ASSERT(!inst.operands.empty(), "Unary instruction requires an operand");
    auto* val{lower_value(inst.operands[0])};
    ASSERT(val, "Unary operand must lower to a non-null LLVM value");

    const bool is_flt{is_float_type(inst, val)};

    switch (inst.kind) {
    case gir::instruction_kind::NEG:
        return is_flt ? builder_.CreateFNeg(val, "negtmp") : builder_.CreateNeg(val, "negtmp");
    case gir::instruction_kind::NOT:    return builder_.CreateNot(val, "nottmp");
    case gir::instruction_kind::BITNOT: return builder_.CreateNot(val, "bitnottmp");
    default:                            UNREACHABLE("Invalid instruction kind in emit_unary");
    }
}

auto llvm_lowering::emit_comparison(const gir::instruction& inst) -> llvm::Value* {
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

    // Unsigned / pointer / boolean comparison
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
    ASSERT(!inst.operands.empty(), "Cast instruction requires an operand");
    ASSERT(inst.type.has_value(), "Cast instruction requires a target type");
    auto* val{lower_value(inst.operands[0])};
    auto* target_ty{types_.translate(*inst.type)};
    ASSERT(val && target_ty, "Cast operand and target type must be valid");

    auto* src_ty{val->getType()};
    switch (inst.kind) {
    case gir::instruction_kind::WIDEN_CAST: {
        const bool src_is_flt{src_ty->isFloatingPointTy()};
        const bool dst_is_flt{target_ty->isFloatingPointTy()};

        if (src_is_flt && dst_is_flt) { return builder_.CreateFPExt(val, target_ty, "fpext"); }
        if (!src_is_flt && dst_is_flt) {
            return is_signed_type(inst) ? builder_.CreateSIToFP(val, target_ty, "sitofp")
                                        : builder_.CreateUIToFP(val, target_ty, "uitofp");
        }
        if (src_is_flt && !dst_is_flt) {
            const bool dst_is_sgn{sema::is_signed_integer(inst.type->get_kind())};
            return dst_is_sgn ? builder_.CreateFPToSI(val, target_ty, "fptosi")
                              : builder_.CreateFPToUI(val, target_ty, "fptoui");
        }
        // Integer widen
        return is_signed_type(inst) ? builder_.CreateSExt(val, target_ty, "sext")
                                    : builder_.CreateZExt(val, target_ty, "zext");
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
    ASSERT(!inst.operands.empty(), "Const instruction requires an operand");
    auto* val{lower_value(inst.operands[0], inst.type ? &*inst.type : nullptr)};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::lower_instruction(const gir::instruction& inst) -> void {
    llvm::Value* result_val{nullptr};

    switch (inst.kind) {
    case gir::instruction_kind::CONST:        result_val = emit_const(inst); break;
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL:
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD:
    case gir::instruction_kind::AND:
    case gir::instruction_kind::OR:
    case gir::instruction_kind::XOR:
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR:          result_val = emit_binary(inst); break;
    case gir::instruction_kind::NEG:
    case gir::instruction_kind::NOT:
    case gir::instruction_kind::BITNOT:       result_val = emit_unary(inst); break;
    case gir::instruction_kind::EQ:
    case gir::instruction_kind::NE:
    case gir::instruction_kind::LT:
    case gir::instruction_kind::LE:
    case gir::instruction_kind::GT:
    case gir::instruction_kind::GE:           result_val = emit_comparison(inst); break;
    case gir::instruction_kind::WIDEN_CAST:
    case gir::instruction_kind::BIT_CAST:
    case gir::instruction_kind::PTR_CAST:
    case gir::instruction_kind::INT_FROM_PTR:
    case gir::instruction_kind::PTR_FROM_INT: result_val = emit_cast(inst); break;
    default:                                  break;
    }

    if (result_val && inst.result) { set_local(*inst.result, result_val); }
}

} // namespace ghoti::codegen
