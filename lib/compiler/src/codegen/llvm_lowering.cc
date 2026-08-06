#include "compiler/codegen/llvm_lowering.hh"

#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Casting.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
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
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"

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

auto llvm_lowering::lower_executable(const gir::module& gir_mod, std::string_view user_main_name)
    -> stdx::box<llvm::Module> {
    is_executable_  = true;
    user_main_name_ = user_main_name;
    for (const auto* global : gir_mod.get_globals()) { lower_global(*global); }
    for (const auto* fn : gir_mod.get_functions()) { declare_function(*fn); }
    for (const auto* fn : gir_mod.get_functions()) { lower_function(*fn); }
    emit_main_entry_wrapper(user_main_name_);
    return std::move(llvm_module_);
}

auto llvm_lowering::emit_main_entry_wrapper(std::string_view user_main_name) -> llvm::Function* {
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

    auto* slice_ty{types_.translate_slice_type()};

    // Windows raw PE entry point
    if (triple.isOSWindows()) {
        // BaseThreadInitThunk calls entry point without C (argc, argv)
        // Construct empty slice [][:0]u8 (args.len = 0) to avoid dereferencing invalid registers
        auto* outer_slice{builder_.CreateAlloca(slice_ty, nullptr, "outer.slice")};
        auto* outer_data{builder_.CreateStructGEP(slice_ty, outer_slice, 0, "outer.data")};
        auto* outer_len{builder_.CreateStructGEP(slice_ty, outer_slice, 1, "outer.len")};
        builder_.CreateStore(llvm::ConstantPointerNull::get(types_.get_ptr_ty()), outer_data);
        builder_.CreateStore(builder_.getInt64(0), outer_len);
        auto* outer_val{builder_.CreateLoad(slice_ty, outer_slice, "outer.val")};

        const bool takes_arg{user_fn->getFunctionType()->getNumParams() == 1};
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
            auto* ret_i32{
                builder_.CreateIntCast(ret_val, types_.get_int32_ty(), true, "main.res.i32")};
            builder_.CreateRet(ret_i32);
        }
        return main_fn;
    }

    auto* argc{main_fn->getArg(0)};
    argc->setName("argc");
    auto* argv{main_fn->getArg(1)};
    argv->setName("argv");

    auto* raw_argc_i64{builder_.CreateSExt(argc, types_.get_int64_ty(), "argc.i64")};

    // Use a fixed 16-element stack buffer to avoid dynamic stack allocation and stack probe issues
    auto* max_args{builder_.getInt64(16)};
    auto* slice_array{builder_.CreateAlloca(slice_ty, max_args, "args.array")};
    auto* argc_i64{builder_.CreateSelect(
        builder_.CreateICmpSLT(raw_argc_i64, max_args), raw_argc_i64, max_args, "argc.bounded")};

    auto* loop_cond{llvm::BasicBlock::Create(context_, "loop.cond", main_fn)};
    auto* loop_body{llvm::BasicBlock::Create(context_, "loop.body", main_fn)};
    auto* loop_inc{llvm::BasicBlock::Create(context_, "loop.inc", main_fn)};
    auto* loop_end{llvm::BasicBlock::Create(context_, "loop.end", main_fn)};

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
    auto* str_len_cond{llvm::BasicBlock::Create(context_, "strlen.cond", main_fn)};
    auto* str_len_body{llvm::BasicBlock::Create(context_, "strlen.body", main_fn)};
    auto* str_len_end{llvm::BasicBlock::Create(context_, "strlen.end", main_fn)};

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
    auto* dest_data_field{builder_.CreateStructGEP(slice_ty, dest_slice_ptr, 0, "dest.data")};
    auto* dest_len_field{builder_.CreateStructGEP(slice_ty, dest_slice_ptr, 1, "dest.len")};
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
    auto* outer_data{builder_.CreateStructGEP(slice_ty, outer_slice, 0, "outer.data")};
    auto* outer_len{builder_.CreateStructGEP(slice_ty, outer_slice, 1, "outer.len")};
    builder_.CreateStore(slice_array, outer_data);
    builder_.CreateStore(argc_i64, outer_len);
    auto* outer_val{builder_.CreateLoad(slice_ty, outer_slice, "outer.val")};

    // Call user main
    const bool takes_arg{user_fn->getFunctionType()->getNumParams() == 1};
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

    return main_fn;
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
    if (const auto it{globals_.find(fn.get_name())}; it != globals_.end()) {
        if (auto* existing{llvm::dyn_cast<llvm::Function>(it->second)}) { return existing; }
    }

    std::string fn_name{fn.get_name()};
    auto        g_linkage{llvm::GlobalValue::ExternalLinkage};
    if (fn.get_linkage() == gir::linkage::INTERNAL) {
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    if (is_executable_ && fn_name == user_main_name_) {
        fn_name   = "_ghoti_main";
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    if (auto* existing = llvm_module_->getFunction(fn_name)) { return existing; }

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

    auto* llvm_fn{llvm::Function::Create(fn_ty, g_linkage, fn_name, llvm_module_.get())};
    llvm_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    llvm_fn->addFnAttr("no-stack-arg-probe", "true");
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

    if (!ptr->getType()->isPointerTy()) {
        if (inst.result) { set_local(*inst.result, ptr); }
        return ptr;
    }

    auto* loaded{builder_.CreateLoad(elem_ty, ptr, inst.is_volatile(), "loadtmp")};
    if (inst.result) { set_local(*inst.result, loaded); }
    return loaded;
}

auto llvm_lowering::emit_store(const gir::instruction& inst) -> void {
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
            source_elem_ty = types_.translate_slice_type();
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

        if (src_is_flt && dst_is_flt) { return builder_.CreateFPCast(val, target_ty, "fpcast"); }
        if (!src_is_flt && dst_is_flt) {
            return is_signed_type(inst) ? builder_.CreateSIToFP(val, target_ty, "sitofp")
                                        : builder_.CreateUIToFP(val, target_ty, "uitofp");
        }
        if (src_is_flt && !dst_is_flt) {
            const bool dst_is_sgn{sema::is_signed_integer(inst.type->get_kind())};
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
                    auto*                       slice_ty{types_.translate_slice_type()};
                    llvm::Value*                slice_val{llvm::UndefValue::get(slice_ty)};
                    const std::vector<unsigned> idx0{0};
                    const std::vector<unsigned> idx1{1};
                    slice_val = builder_.CreateInsertValue(slice_val, arg_val, idx0);
                    slice_val = builder_.CreateInsertValue(
                        slice_val, builder_.getInt64(static_cast<u64>(arr_data->len)), idx1);
                    arg_val = slice_val;
                }
            } else if (op.type &&
                       (op.type->get_kind() == sema::type_kind::STRUCT ||
                        op.type->get_kind() == sema::type_kind::UNION ||
                        op.type->get_kind() == sema::type_kind::SLICE) &&
                       arg_val->getType()->isPointerTy()) {
                auto* llvm_struct_ty{types_.translate(*op.type)};
                arg_val = builder_.CreateLoad(llvm_struct_ty, arg_val, "struct_arg");
            }
            args.emplace_back(arg_val);
        }
        const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_};

        auto* call_inst{builder_.CreateCall(callee_fn, args, is_void ? "" : "calltmp")};
        if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
        return call_inst;
    }

    ASSERT(!inst.operands.empty(), "Indirect call requires callee operand");
    auto* callee_val{lower_value(inst.operands[0])};
    ASSERT(inst.operands[0].type, "Indirect callee must have function type");
    const sema::type* ind_target{inst.operands[0].type.has_value() ? &inst.operands[0].type.value()
                                                                   : nullptr};
    if (const auto ref{ind_target->get_data().as_opt<sema::types::reference>()}) {
        ind_target = &ref->underlying;
    }
    if (const auto ptr{ind_target->get_data().as_opt<sema::types::pointer>()}) {
        ind_target = &ptr->underlying;
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
             operand.type->get_kind() == sema::type_kind::SLICE) &&
            arg_val->getType()->isPointerTy()) {
            auto* llvm_struct_ty{types_.translate(*operand.type)};
            arg_val = builder_.CreateLoad(llvm_struct_ty, arg_val, "struct_arg");
        }
        args.emplace_back(arg_val);
    }

    const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_};
    auto*      call_inst{builder_.CreateCall(fn_ty, callee_val, args, is_void ? "" : "calltmp")};
    if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
    return call_inst;
}

auto llvm_lowering::emit_builtin_call(const gir::instruction& inst) -> llvm::Value* {
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
        case syntax::token_type_t::BUILTIN_PANIC: {
            auto* trap_fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
            builder_.CreateCall(trap_fn, {});
            builder_.CreateUnreachable();
            return nullptr;
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

        // Math intrinsics
        case syntax::token_type_t::BUILTIN_SQRT: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::sqrt, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_SIN: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::sin, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_COS: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::cos, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_EXP: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::exp, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_EXP2: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::exp2, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_LOG: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::log, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_LOG2: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::log2, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_LOG10: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::log10, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_FLOOR: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::floor, {val->getType()})};
                return builder_.CreateCall(fn, {val});
            }
            return nullptr;
        }
        case syntax::token_type_t::BUILTIN_CEIL: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::ceil, {val->getType()})};
                return builder_.CreateCall(fn, {val});
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
        default: break;
        }
    }
    return emit_call(inst);
}

auto llvm_lowering::emit_ret(const gir::instruction& inst) -> void {
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
