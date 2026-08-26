#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Casting.h>
#include <stdx/arena.hh>

#include "compiler/codegen/type_translator.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Type translate primitive types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};

    auto& i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto& i64_t{*pool[{sema::type_kind::I64, sema::types::mut::CONSTANT}]};
    auto& isize_t{*pool[{sema::type_kind::ISIZE, sema::types::mut::CONSTANT}]};
    auto& u8_t{*pool[{sema::type_kind::U8, sema::types::mut::CONSTANT}]};
    auto& u32_t{*pool[{sema::type_kind::U32, sema::types::mut::CONSTANT}]};
    auto& u64_t{*pool[{sema::type_kind::U64, sema::types::mut::CONSTANT}]};
    auto& usize_t{*pool[{sema::type_kind::USIZE, sema::types::mut::CONSTANT}]};
    auto& f32_t{*pool[{sema::type_kind::F32, sema::types::mut::CONSTANT}]};
    auto& f64_t{*pool[{sema::type_kind::F64, sema::types::mut::CONSTANT}]};
    auto& bool_t{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};
    auto& void_t{*pool[{sema::type_kind::VOID_, sema::types::mut::CONSTANT}]};

    CHECK(translator.translate(i32_t) == llvm::Type::getInt32Ty(context));
    CHECK(translator.translate(i64_t) == llvm::Type::getInt64Ty(context));
    CHECK(translator.translate(isize_t) == llvm::Type::getInt64Ty(context));
    CHECK(translator.translate(u8_t) == llvm::Type::getInt8Ty(context));
    CHECK(translator.translate(u32_t) == llvm::Type::getInt32Ty(context));
    CHECK(translator.translate(u64_t) == llvm::Type::getInt64Ty(context));
    CHECK(translator.translate(usize_t) == llvm::Type::getInt64Ty(context));
    CHECK(translator.translate(f32_t) == llvm::Type::getFloatTy(context));
    CHECK(translator.translate(f64_t) == llvm::Type::getDoubleTy(context));
    CHECK(translator.translate(bool_t) == llvm::Type::getInt1Ty(context));
    CHECK(translator.translate(void_t) == llvm::Type::getVoidTy(context));
}

TEST_CASE("Type translate pointer and reference types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};

    auto& i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto& ptr_t{*pool[{sema::type_kind::POINTER, sema::types::mut::CONSTANT, &i32_t}]};
    ptr_t.resolve<sema::types::pointer>(i32_t);

    auto& ref_t{*pool[{sema::type_kind::REFERENCE, sema::types::mut::CONSTANT, &i32_t}]};
    ref_t.resolve<sema::types::reference>(i32_t);

    auto* ptr_llvm{translator.translate(ptr_t)};
    CHECK(ptr_llvm->isPointerTy());
    CHECK(ptr_llvm == llvm::PointerType::get(context, 0));

    auto* ref_llvm{translator.translate(ref_t)};
    CHECK(ref_llvm->isPointerTy());
    CHECK(ref_llvm == llvm::PointerType::get(context, 0));
}

TEST_CASE("Type translate array and slice types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};

    auto& i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto& arr_t{*pool[{sema::type_kind::ARRAY, sema::types::mut::CONSTANT, &i32_t, 5UZ}]};
    arr_t.resolve<sema::types::array>(i32_t, 5UZ, false);

    auto* arr_llvm = translator.translate(arr_t);
    REQUIRE(arr_llvm->isArrayTy());
    auto* arr_ty = llvm::cast<llvm::ArrayType>(arr_llvm);
    CHECK(arr_ty->getNumElements() == 5);
    CHECK(arr_ty->getElementType() == llvm::Type::getInt32Ty(context));

    auto& slice_t{*pool[{sema::type_kind::SLICE, sema::types::mut::CONSTANT, &i32_t}]};
    slice_t.resolve<sema::types::slice>(i32_t, false);

    auto* slice_llvm = translator.translate(slice_t);
    REQUIRE(slice_llvm->isStructTy());
    auto* slice_struct = llvm::cast<llvm::StructType>(slice_llvm);
    REQUIRE(slice_struct->getNumElements() == 2);
    CHECK(slice_struct->getElementType(0)->isPointerTy());
    CHECK(slice_struct->getElementType(1)->isIntegerTy(64));
}

TEST_CASE("Type translate function types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};

    auto& i32_t{*pool[{sema::type_kind::I32, sema::types::mut::CONSTANT}]};
    auto& f64_t{*pool[{sema::type_kind::F64, sema::types::mut::CONSTANT}]};
    auto& bool_t{*pool[{sema::type_kind::BOOL, sema::types::mut::CONSTANT}]};

    auto params{arena.make_span<sema::type*>(2)};
    params[0] = &i32_t;
    params[1] = &f64_t;

    auto& fn_t{*pool[{sema::type_kind::FUNCTION, sema::types::mut::CONSTANT}]};
    fn_t.resolve<sema::types::function>(false, params, bool_t, false);
    auto* fn_llvm{translator.translate(fn_t)};
    CHECK(fn_llvm->isPointerTy());

    auto* fn_ty{translator.translate_function_type(fn_t.get_data().as<sema::types::function>())};
    CHECK(fn_ty->getReturnType() == llvm::Type::getInt1Ty(context));
    REQUIRE(fn_ty->getNumParams() == 2);
    CHECK(fn_ty->getParamType(0) == llvm::Type::getInt32Ty(context));
    CHECK(fn_ty->getParamType(1) == llvm::Type::getDoubleTy(context));
    CHECK_FALSE(fn_ty->isVarArg());
}

TEST_CASE("Type translate struct and enum types from parsed programs") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Color := enum {
            RED,
            GREEN,
            BLUE,
        };

        const Point := struct {
            x: i32,
            y: i32,
            color: Color,
        };
    )")};

    const auto [color_sym, color_data, color_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("Color", idx)};
    auto* color_llvm{translator.translate(color_type)};
    CHECK(color_llvm == llvm::Type::getInt32Ty(context));

    const auto [point_sym, point_data, point_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("Point", idx)};
    auto* point_llvm{translator.translate(point_type)};
    REQUIRE(point_llvm->isStructTy());
    auto* point_struct{llvm::cast<llvm::StructType>(point_llvm)};
    REQUIRE(point_struct->getNumElements() == 3);
    CHECK(point_struct->getElementType(0) == llvm::Type::getInt32Ty(context));
    CHECK(point_struct->getElementType(1) == llvm::Type::getInt32Ty(context));
    CHECK(point_struct->getElementType(2) == llvm::Type::getInt32Ty(context));
}

TEST_CASE("Type translate tagged union types from parsed programs") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const Value := union {
            none: void,
            small: i32,
            big: f64,
        };
    )")};

    const auto [val_sym, val_data, val_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("Value", idx)};
    auto* val_llvm{translator.translate(val_type)};
    REQUIRE(val_llvm->isStructTy());
    auto* val_struct{llvm::cast<llvm::StructType>(val_llvm)};
    REQUIRE(val_struct->getNumElements() == 2);
    CHECK(val_struct->getElementType(0)->isIntegerTy(32));
    REQUIRE(val_struct->getElementType(1)->isArrayTy());
    auto* payload_arr{llvm::cast<llvm::ArrayType>(val_struct->getElementType(1))};
    CHECK(payload_arr->getNumElements() == 8);
    CHECK(payload_arr->getElementType()->isIntegerTy(8));
}

TEST_CASE("Type translate untagged extern union types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const RawUnion := extern union {
            i: i32,
            f: f64,
        };
    )")};

    const auto [union_sym, union_data, union_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("RawUnion", idx)};
    auto* union_llvm{translator.translate(union_type)};
    REQUIRE(union_llvm->isArrayTy());
    auto* arr_ty{llvm::cast<llvm::ArrayType>(union_llvm)};
    CHECK(arr_ty->getNumElements() == 8);
    CHECK(arr_ty->getElementType()->isIntegerTy(8));
}

TEST_CASE("Untagged union payload size follows the module's target data layout") {
    llvm::LLVMContext context;
    llvm::Module      mod{"test", context};
    mod.setDataLayout("e-p:32:32"); // 32-bit pointers, unlike this host build
    codegen::type_translator translator{context, mod};

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const RawUnion := extern union {
            p: ^i32,
        };
    )")};

    const auto [union_sym, union_data, union_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("RawUnion", idx)};
    auto* union_llvm{translator.translate(union_type)};
    REQUIRE(union_llvm->isArrayTy());
    CHECK(llvm::cast<llvm::ArrayType>(union_llvm)->getNumElements() == 4);
}

TEST_CASE("usize/isize/slice-length follow the module's target data layout") {
    llvm::LLVMContext context;
    llvm::Module      mod{"test", context};
    mod.setDataLayout("e-p:32:32"); // 32-bit pointers, unlike this host build
    codegen::type_translator translator{context, mod};

    sema::arena_alloc arena;
    sema::type_pool   pool{arena};

    auto& isize_t{*pool[{sema::type_kind::ISIZE, sema::types::mut::CONSTANT}]};
    auto& usize_t{*pool[{sema::type_kind::USIZE, sema::types::mut::CONSTANT}]};

    CHECK(translator.translate(isize_t) == llvm::Type::getInt32Ty(context));
    CHECK(translator.translate(usize_t) == llvm::Type::getInt32Ty(context));

    auto* slice_ty{translator.translate_slice_type()};
    REQUIRE(slice_ty->getNumElements() == 2);
    CHECK(slice_ty->getElementType(1)->isIntegerTy(32));
    CHECK(mod.getDataLayout().getTypeAllocSize(slice_ty).getFixedValue() == 8);
}

TEST_CASE("Type translate packed struct types") {
    llvm::LLVMContext        context;
    llvm::Module             mod{"test", context};
    codegen::type_translator translator{context, mod};

    auto [ctx, idx]{helpers::resolve_and_check(R"(
        const PackedHeader := packed struct {
            flag: u8,
            val: u32,
        };
    )")};

    const auto [packed_sym, packed_data, packed_type]{
        ctx->get_type_sym_info<sema::symbols::node_t>("PackedHeader", idx)};
    auto* packed_llvm{translator.translate(packed_type)};
    REQUIRE(packed_llvm->isStructTy());
    auto* struct_ty{llvm::cast<llvm::StructType>(packed_llvm)};
    CHECK(struct_ty->isPacked());
    REQUIRE(struct_ty->getNumElements() == 2);
    CHECK(struct_ty->getElementType(0)->isIntegerTy(8));
    CHECK(struct_ty->getElementType(1)->isIntegerTy(32));
}

} // namespace ghoti::tests
