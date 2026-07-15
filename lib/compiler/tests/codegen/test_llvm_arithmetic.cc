#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Casting.h>
#include <stdx/arena.hh>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("LLVM lowering integer and float constants") {
    llvm::LLVMContext      context;
    codegen::llvm_lowering lowering{context, "test_consts"};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             f64_t{*pool[{sema::type_kind::F64, sema::types::mut::CONSTANT}]};
    auto&             bool_t{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};

    auto* c_i32{lowering.lower_value(gir::value{i64{42}, &i32_t})};
    REQUIRE(c_i32 != nullptr);
    CHECK(c_i32->getType()->isIntegerTy(32));
    CHECK(llvm::cast<llvm::ConstantInt>(c_i32)->getSExtValue() == 42);

    auto* c_f64{lowering.lower_value(gir::value{3.14, &f64_t})};
    REQUIRE(c_f64 != nullptr);
    CHECK(c_f64->getType()->isDoubleTy());

    auto* c_bool{lowering.lower_value(gir::value{true, &bool_t})};
    REQUIRE(c_bool != nullptr);
    CHECK(c_bool->getType()->isIntegerTy(1));
    CHECK(llvm::cast<llvm::ConstantInt>(c_bool)->getZExtValue() == 1);
}

TEST_CASE("LLVM lowering binary arithmetic instructions") {
    llvm::LLVMContext      context;
    codegen::llvm_lowering lowering{context, "test_binary"};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             u32_t{*pool[{sema::type_kind::U32, sema::types::mut::CONSTANT}]};
    auto&             f64_t{*pool[{sema::type_kind::F64, sema::types::mut::CONSTANT}]};

    // fn test_fn(a: i32, b: i32, u_a: u32, u_b: u32, f_a: f64, f_b: f64)
    auto* fn_ty{llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                        {llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt32Ty(context),
                                         llvm::Type::getDoubleTy(context),
                                         llvm::Type::getDoubleTy(context)},
                                        false)};
    auto* fn{llvm::Function::Create(
        fn_ty, llvm::Function::ExternalLinkage, "test_fn", &lowering.module())};
    auto* bb{llvm::BasicBlock::Create(context, "entry", fn)};
    lowering.builder().SetInsertPoint(bb);

    lowering.set_local(gir::local_id::make_param(0), fn->getArg(0));
    lowering.set_local(gir::local_id::make_param(1), fn->getArg(1));
    lowering.set_local(gir::local_id::make_param(2), fn->getArg(2));
    lowering.set_local(gir::local_id::make_param(3), fn->getArg(3));
    lowering.set_local(gir::local_id::make_param(4), fn->getArg(4));
    lowering.set_local(gir::local_id::make_param(5), fn->getArg(5));

    gir::instruction add_inst{
        .kind   = gir::instruction_kind::ADD,
        .type   = i32_t,
        .result = gir::local_id::make_temp(0),
        .operands =
            {
                gir::value{gir::local_id::make_param(0), &i32_t},
                gir::value{gir::local_id::make_param(1), &i32_t},
            },
    };
    lowering.lower_instruction(add_inst);
    auto* add_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(0)))};
    REQUIRE(add_val->getType()->isIntegerTy(32));
    REQUIRE(llvm::isa<llvm::BinaryOperator>(add_val));
    CHECK(llvm::cast<llvm::BinaryOperator>(add_val)->getOpcode() == llvm::Instruction::Add);

    gir::instruction sdiv_inst{
        .kind   = gir::instruction_kind::DIV,
        .type   = i32_t,
        .result = gir::local_id::make_temp(1),
        .operands =
            {
                gir::value{gir::local_id::make_param(0), &i32_t},
                gir::value{gir::local_id::make_param(1), &i32_t},
            },
    };
    lowering.lower_instruction(sdiv_inst);
    auto* sdiv_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(1)))};
    REQUIRE(llvm::isa<llvm::BinaryOperator>(sdiv_val));
    CHECK(llvm::cast<llvm::BinaryOperator>(sdiv_val)->getOpcode() == llvm::Instruction::SDiv);

    gir::instruction udiv_inst{
        .kind   = gir::instruction_kind::DIV,
        .type   = u32_t,
        .result = gir::local_id::make_temp(2),
        .operands =
            {
                gir::value{gir::local_id::make_param(2), &u32_t},
                gir::value{gir::local_id::make_param(3), &u32_t},
            },
    };
    lowering.lower_instruction(udiv_inst);
    auto* udiv_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(2)))};
    REQUIRE(llvm::isa<llvm::BinaryOperator>(udiv_val));
    CHECK(llvm::cast<llvm::BinaryOperator>(udiv_val)->getOpcode() == llvm::Instruction::UDiv);

    gir::instruction fadd_inst{
        .kind   = gir::instruction_kind::ADD,
        .type   = f64_t,
        .result = gir::local_id::make_temp(3),
        .operands =
            {
                gir::value{gir::local_id::make_param(4), &f64_t},
                gir::value{gir::local_id::make_param(5), &f64_t},
            },
    };
    lowering.lower_instruction(fadd_inst);
    auto* fadd_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(3)))};
    REQUIRE(llvm::isa<llvm::BinaryOperator>(fadd_val));
    CHECK(llvm::cast<llvm::BinaryOperator>(fadd_val)->getOpcode() == llvm::Instruction::FAdd);
}

TEST_CASE("LLVM lowering unary and comparison instructions") {
    llvm::LLVMContext      context;
    codegen::llvm_lowering lowering{context, "test_unary_cmp"};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             bool_t{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};

    auto* fn_ty{llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                        {llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt32Ty(context),
                                         llvm::Type::getInt1Ty(context)},
                                        false)};
    auto* fn{llvm::Function::Create(
        fn_ty, llvm::Function::ExternalLinkage, "test_fn", &lowering.module())};
    auto* bb{llvm::BasicBlock::Create(context, "entry", fn)};
    lowering.builder().SetInsertPoint(bb);

    lowering.set_local(gir::local_id::make_param(0), fn->getArg(0));
    lowering.set_local(gir::local_id::make_param(1), fn->getArg(1));
    lowering.set_local(gir::local_id::make_param(2), fn->getArg(2));

    gir::instruction neg_inst{
        .kind     = gir::instruction_kind::NEG,
        .type     = i32_t,
        .result   = gir::local_id::make_temp(0),
        .operands = {gir::value{gir::local_id::make_param(0), &i32_t}},
    };
    lowering.lower_instruction(neg_inst);
    auto* neg_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(0)))};
    CHECK(neg_val->getType()->isIntegerTy(32));

    gir::instruction not_inst{
        .kind     = gir::instruction_kind::NOT,
        .type     = bool_t,
        .result   = gir::local_id::make_temp(1),
        .operands = {gir::value{gir::local_id::make_param(2), &bool_t}},
    };
    lowering.lower_instruction(not_inst);
    auto* not_val = UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(1)));
    CHECK(not_val->getType()->isIntegerTy(1));
    REQUIRE(llvm::isa<llvm::BinaryOperator>(not_val));

    gir::instruction cmp_inst{
        .kind   = gir::instruction_kind::LT,
        .type   = bool_t,
        .result = gir::local_id::make_temp(2),
        .operands =
            {
                gir::value{gir::local_id::make_param(0), &i32_t},
                gir::value{gir::local_id::make_param(1), &i32_t},
            },
    };
    lowering.lower_instruction(cmp_inst);
    auto* cmp_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(2)))};
    REQUIRE(llvm::isa<llvm::ICmpInst>(cmp_val));
    CHECK(llvm::cast<llvm::ICmpInst>(cmp_val)->getPredicate() == llvm::CmpInst::ICMP_SLT);
}

TEST_CASE("LLVM lowering cast instructions") {
    llvm::LLVMContext      context;
    codegen::llvm_lowering lowering{context, "test_casts"};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};
    auto&             i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto&             i64_t{*pool[{sema::type_kind::I64, sema::types::mut::CONSTANT}]};
    auto&             u8_t{*pool[{sema::type_kind::U8, sema::types::mut::CONSTANT}]};
    auto&             u32_t{*pool[{sema::type_kind::U32, sema::types::mut::CONSTANT}]};
    auto&             ptr_t{*pool[{sema::type_kind::POINTER, sema::types::mut::CONSTANT, &i32_t}]};

    auto* fn_ty = llvm::FunctionType::get(llvm::Type::getVoidTy(context),
                                          {llvm::Type::getInt32Ty(context),
                                           llvm::Type::getInt8Ty(context),
                                           llvm::Type::getInt64Ty(context)},
                                          false);
    auto* fn{llvm::Function::Create(
        fn_ty, llvm::Function::ExternalLinkage, "test_fn", &lowering.module())};
    auto* bb{llvm::BasicBlock::Create(context, "entry", fn)};
    lowering.builder().SetInsertPoint(bb);

    lowering.set_local(gir::local_id::make_param(0), fn->getArg(0));
    lowering.set_local(gir::local_id::make_param(1), fn->getArg(1));
    lowering.set_local(gir::local_id::make_param(2), fn->getArg(2));

    // Signed Widen Cast: i32 -> i64 (SExt)
    gir::instruction sext_inst{
        .kind     = gir::instruction_kind::WIDEN_CAST,
        .type     = i64_t,
        .result   = gir::local_id::make_temp(0),
        .operands = {gir::value{gir::local_id::make_param(0), &i32_t}},
    };
    lowering.lower_instruction(sext_inst);
    auto* sext_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(0)))};
    REQUIRE(llvm::isa<llvm::SExtInst>(sext_val));
    CHECK(sext_val->getType()->isIntegerTy(64));

    // Unsigned Widen Cast: u8 -> u32 (ZExt)
    gir::instruction zext_inst{
        .kind     = gir::instruction_kind::WIDEN_CAST,
        .type     = u32_t,
        .result   = gir::local_id::make_temp(1),
        .operands = {gir::value{gir::local_id::make_param(1), &u8_t}},
    };
    lowering.lower_instruction(zext_inst);
    auto* zext_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(1)))};
    REQUIRE(llvm::isa<llvm::ZExtInst>(zext_val));
    CHECK(zext_val->getType()->isIntegerTy(32));

    // Pointer from Int: i64 -> ^i32 (IntToPtr)
    gir::instruction ptr_from_int_inst{
        .kind     = gir::instruction_kind::PTR_FROM_INT,
        .type     = ptr_t,
        .result   = gir::local_id::make_temp(2),
        .operands = {gir::value{gir::local_id::make_param(2), &i64_t}},
    };
    lowering.lower_instruction(ptr_from_int_inst);
    auto* pfi_val{UNWRAP(lowering.get_local_opt(gir::local_id::make_temp(2)))};
    REQUIRE(llvm::isa<llvm::IntToPtrInst>(pfi_val));
    CHECK(pfi_val->getType()->isPointerTy());
}

} // namespace ghoti::tests
