#include "compiler/codegen/llvm_lowering.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Casting.h>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
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

auto llvm_lowering::lower(const gir::module& gir_mod) -> stdx::box<llvm::Module> {
    for (const auto* global : gir_mod.get_globals()) { lower_global(*global); }
    for (const auto* fn : gir_mod.get_functions()) { declare_function(*fn); }
    for (const auto* fn : gir_mod.get_functions()) { lower_function(*fn); }
    return std::move(llvm_module_);
}

auto llvm_lowering::lower_global(const gir::global_decl& g) -> llvm::GlobalVariable* {
    if (auto* existing{llvm_module_->getGlobalVariable(g.name)}) { return existing; }

    auto*      g_type{types_.translate(g.type)};
    const bool is_const{g.is_constant};
    const auto g_linkage{(g.linkage == gir::linkage::INTERNAL)
                             ? llvm::GlobalValue::InternalLinkage
                             : llvm::GlobalValue::ExternalLinkage};

    stdx::option<llvm::Constant&> init;
    if (g.init_value) {
        auto* init_v{lower_value(*g.init_value, &g.type)};
        if (init_v && llvm::isa<llvm::Constant>(init_v)) {
            init.emplace(llvm::cast<llvm::Constant>(init_v));
        }
    }
    if (!init) { init.emplace(llvm::Constant::getNullValue(g_type)); }

    auto* gvar{
        new llvm::GlobalVariable{*llvm_module_, g_type, is_const, g_linkage, init.get(), g.name}};
    globals_[g.name] = gvar;
    return gvar;
}

auto llvm_lowering::declare_function(const gir::function& fn) -> llvm::Function* {
    if (auto* existing = llvm_module_->getFunction(fn.get_name())) { return existing; }

    auto* fn_ty{
        types_.translate_function_type(fn.get_type().get_data().as<sema::types::function>())};
    auto g_linkage{llvm::GlobalValue::ExternalLinkage};
    if (fn.get_linkage() == gir::linkage::INTERNAL) {
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    auto* llvm_fn{llvm::Function::Create(fn_ty, g_linkage, fn.get_name(), llvm_module_.get())};
    for (usize i{0}; const auto& param : fn.get_params()) {
        auto* arg{llvm_fn->getArg(static_cast<u32>(i++))};
        arg->setName(param->name);
    }

    globals_[fn.get_name()] = llvm_fn;
    return llvm_fn;
}

auto llvm_lowering::lower_function(const gir::function& fn) -> llvm::Function* {
    auto* llvm_fn{declare_function(fn)};
    if (fn.get_linkage() == gir::linkage::EXTERN || fn.get_segments().empty()) { return llvm_fn; }
    clear_locals();

    // Bind parameters
    for (usize i{0}; const auto& param : fn.get_params()) {
        auto* arg{llvm_fn->getArg(static_cast<u32>(i++))};
        set_local(param->id, arg);
    }

    // Pre-allocate basic blocks for all segments
    for (const auto* seg : fn.get_segments()) {
        auto* bb{llvm::BasicBlock::Create(
            context_, fmt::format("seg_{}", std::to_underlying(seg->get_id())), llvm_fn)};
        segment_blocks_[seg->get_id()] = bb;
    }

    // Lower each segment
    for (const auto* seg : fn.get_segments()) {
        auto* bb{segment_blocks_[seg->get_id()]};
        builder_.SetInsertPoint(bb);

        for (const auto* inst : seg->get_instructions()) { lower_instruction(*inst); }
        if (bb->getTerminator() == nullptr) {
            if (fn.get_type().get_data().as<sema::types::function>().return_type.get_kind() ==
                sema::type_kind::VOID) {
                builder_.CreateRetVoid();
            } else {
                builder_.CreateUnreachable();
            }
        }
    }

    return llvm_fn;
}

auto llvm_lowering::lower_value(const gir::value& val, const sema::type* expected_type)
    -> llvm::Value* {
    return val.data.visit(
        [this](gir::local_id loc) -> llvm::Value* {
            const auto it{locals_.find(loc)};
            if (it != locals_.end()) { return it->second; }
            return nullptr;
        },
        [this, &val, expected_type](i64 i) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return llvm::ConstantInt::get(ty, static_cast<u64>(i), true);
        },
        [this, &val, expected_type](u64 u) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_int64_ty()};
            return llvm::ConstantInt::get(ty, u, false);
        },
        [this, &val, expected_type](f64 f) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
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
            if (ty->isVoidTy()) { return nullptr; }
            return llvm::UndefValue::get(ty);
        },
        [](gir::void_val) -> llvm::Value* { return nullptr; },
        [](const stdx::option<sema::type&>&) -> llvm::Value* { return nullptr; });
}

auto llvm_lowering::lower_instruction(const gir::instruction& inst) -> void {
    llvm::Value* result_val{nullptr};

    switch (inst.kind) {
    case gir::instruction_kind::ALLOCA:          result_val = emit_alloca(inst); break;
    case gir::instruction_kind::LOAD:            result_val = emit_load(inst); break;
    case gir::instruction_kind::STORE:           emit_store(inst); return;
    case gir::instruction_kind::GET_ELEMENT_PTR: result_val = emit_get_element_ptr(inst); break;
    case gir::instruction_kind::ADDRESS_OF:      result_val = emit_address_of(inst); break;
    case gir::instruction_kind::DEREF:           result_val = emit_deref(inst); break;
    case gir::instruction_kind::CONST:           result_val = emit_const(inst); break;
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
    case gir::instruction_kind::RET:             emit_ret(inst); return;
    case gir::instruction_kind::GOTO:            emit_goto(inst); return;
    case gir::instruction_kind::COND_GOTO:       emit_cond_goto(inst); return;
    case gir::instruction_kind::UNREACHABLE:     emit_unreachable(); return;
    }

    if (result_val && inst.result) { set_local(*inst.result, result_val); }
}

auto llvm_lowering::emit_alloca(const gir::instruction& inst) -> llvm::Value* {
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

    auto* loaded{builder_.CreateLoad(elem_ty, ptr, "loadtmp")};
    if (inst.result) { set_local(*inst.result, loaded); }
    return loaded;
}

auto llvm_lowering::emit_store(const gir::instruction& inst) -> void {
    if (inst.operands.size() >= 2) {
        auto* dest_ptr{lower_value(inst.operands[0])};
        auto* val{lower_value(inst.operands[1], inst.type ? &*inst.type : nullptr)};
        if (!dest_ptr || !val || val->getType()->isVoidTy()) { return; }
        builder_.CreateStore(val, dest_ptr);
    } else if (inst.result && !inst.operands.empty()) {
        auto* dest_ptr{lower_value(gir::value{*inst.result})};
        auto* val{lower_value(inst.operands[0], inst.type ? &*inst.type : nullptr)};
        if (!dest_ptr || !val || val->getType()->isVoidTy()) { return; }
        builder_.CreateStore(val, dest_ptr);
    }
}

auto llvm_lowering::emit_get_element_ptr(const gir::instruction& inst) -> llvm::Value* {
    ASSERT(inst.operands.size() >= 2, "GEP requires base and at least one index");
    auto* base_ptr{lower_value(inst.operands[0])};
    ASSERT(base_ptr, "GEP base pointer must be non-null");

    llvm::Type*               source_elem_ty{nullptr};
    std::vector<llvm::Value*> indices;

    if (inst.operands[0].type) {
        const auto& base_type{*inst.operands[0].type};
        switch (base_type.get_kind()) {
        case sema::type_kind::STRUCT:
        case sema::type_kind::UNION:
            source_elem_ty = types_.translate(base_type);
            indices.emplace_back(builder_.getInt32(0));
            for (const auto& operand : inst.operands) {
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
            for (const auto& operand : inst.operands) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        case sema::type_kind::SLICE:
            source_elem_ty = types_.translate_slice_type();
            indices.emplace_back(builder_.getInt32(0));
            for (const auto& operand : inst.operands) {
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

            for (const auto& operand : inst.operands) {
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

            for (const auto& operand : inst.operands) {
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

            for (const auto& operand : inst.operands) {
                indices.emplace_back(lower_value(operand));
            }
            break;
        }
    } else if (inst.type) {
        source_elem_ty = types_.translate(*inst.type);
        for (const auto& operand : inst.operands) { indices.emplace_back(lower_value(operand)); }
    } else {
        source_elem_ty = types_.get_int8_ty();
        for (const auto& operand : inst.operands) { indices.emplace_back(lower_value(operand)); }
    }

    auto* gep{builder_.CreateInBoundsGEP(source_elem_ty, base_ptr, indices, "geptmp")};
    if (inst.result) { set_local(*inst.result, gep); }
    return gep;
}

auto llvm_lowering::emit_address_of(const gir::instruction& inst) -> llvm::Value* {
    ASSERT(!inst.operands.empty(), "AddressOf requires an operand");
    auto* val{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::emit_deref(const gir::instruction& inst) -> llvm::Value* {
    ASSERT(!inst.operands.empty(), "Deref requires an operand");
    auto* ptr{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, ptr); }
    return ptr;
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
    ASSERT(inst.type, "Cast instruction requires a target type");
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

auto llvm_lowering::emit_call(const gir::instruction& inst) -> llvm::Value* {
    if (inst.callee_name) {
        auto* callee_fn{llvm_module_->getFunction(*inst.callee_name)};
        ASSERT(callee_fn, "Callee function not found in module");
        std::vector<llvm::Value*> args;
        args.reserve(inst.operands.size());
        for (const auto& op : inst.operands) { args.emplace_back(lower_value(op)); }
        const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID};
        auto*      call_inst{builder_.CreateCall(callee_fn, args, is_void ? "" : "calltmp")};
        if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
        return call_inst;
    }

    ASSERT(!inst.operands.empty(), "Indirect call requires callee operand");
    auto* callee_val{lower_value(inst.operands[0])};
    ASSERT(inst.operands[0].type, "Indirect callee must have function type");
    auto* fn_ty{types_.translate_function_type(
        inst.operands[0].type->get_data().as<sema::types::function>())};

    std::vector<llvm::Value*> args;
    args.reserve(inst.operands.size() - 1);
    for (const auto& operand : inst.operands) { args.emplace_back(lower_value(operand)); }

    const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID};
    auto*      call_inst{builder_.CreateCall(fn_ty, callee_val, args, is_void ? "" : "calltmp")};
    if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
    return call_inst;
}

auto llvm_lowering::emit_builtin_call(const gir::instruction& inst) -> llvm::Value* {
    if (inst.callee_name) {
        if (*inst.callee_name == "@panic" || *inst.callee_name == "panic") {
            auto* trap_fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
            builder_.CreateCall(trap_fn, {});
            builder_.CreateUnreachable();
            return nullptr;
        }
    }
    return emit_call(inst);
}

auto llvm_lowering::emit_ret(const gir::instruction& inst) -> void {
    if (inst.operands.empty() || (inst.type && inst.type->get_kind() == sema::type_kind::VOID)) {
        builder_.CreateRetVoid();
    } else {
        auto* val{lower_value(inst.operands[0])};
        if (!val || val->getType()->isVoidTy()) {
            builder_.CreateRetVoid();
        } else {
            builder_.CreateRet(val);
        }
    }
}

auto llvm_lowering::emit_goto(const gir::instruction& inst) -> void {
    ASSERT(inst.target_segment, "GOTO instruction requires a target segment");
    const auto it{segment_blocks_.find(*inst.target_segment)};
    ASSERT(it != segment_blocks_.end(), "Target segment block not found");
    builder_.CreateBr(it->second);
}

auto llvm_lowering::emit_cond_goto(const gir::instruction& inst) -> void {
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
