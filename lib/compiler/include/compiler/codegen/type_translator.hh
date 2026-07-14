#pragma once

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <stdx/utility.hh>

#include "compiler/sema/type.hh"

namespace ghoti::codegen {

class type_translator {
  public:
    explicit type_translator(llvm::LLVMContext& context) noexcept : context_{context} {}
    ~type_translator() = default;
    MAKE_PINNED(type_translator);

    MAKE_DEDUCING_GETTER(context);
    [[nodiscard]] auto translate(const sema::type& type) -> llvm::Type*;
    [[nodiscard]] auto translate_function_type(const sema::types::function& fn)
        -> llvm::FunctionType*;
    [[nodiscard]] auto translate_slice_type() -> llvm::StructType*;

    [[nodiscard]] auto get_int8_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int32_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int64_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int1_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_float_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_double_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_void_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_ptr_ty() const noexcept -> llvm::PointerType*;

  private:
    auto translate_slice(const sema::types::slice& s) -> llvm::Type*;
    auto translate_array(const sema::types::array& a) -> llvm::Type*;
    auto translate_struct(const sema::types::struct_t& s, const sema::type& original)
        -> llvm::Type*;
    auto translate_union(const sema::types::union_t& u, const sema::type& original) -> llvm::Type*;
    auto translate_enum(const sema::types::enum_t& e) -> llvm::Type*;

  private:
    llvm::LLVMContext&                                           context_;
    ankerl::unordered_dense::map<const sema::type*, llvm::Type*> struct_cache_;
    ankerl::unordered_dense::map<const sema::type*, llvm::Type*> union_cache_;
};

} // namespace ghoti::codegen
