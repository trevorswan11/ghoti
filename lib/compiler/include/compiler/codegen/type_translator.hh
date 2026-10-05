#pragma once

#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/sema/type.hh"

namespace ghoti::codegen {

class type_translator {
  public:
    // `module`'s data layout is read for target-correct union sizing; needn't be final yet.
    explicit type_translator(llvm::LLVMContext& context, llvm::Module& module) noexcept
        : context_{context}, module_{module} {}
    ~type_translator() = default;
    MAKE_PINNED(type_translator);

    MAKE_DEDUCING_GETTER(context);
    [[nodiscard]] auto translate(const sema::type& type) -> llvm::Type*;
    [[nodiscard]] auto translate_function_type(const sema::types::function& fn)
        -> llvm::FunctionType*;
    [[nodiscard]] auto translate_slice_type() -> llvm::StructType*;
    [[nodiscard]] auto translate_dyn_fat_ptr() -> llvm::StructType*;

    [[nodiscard]] auto get_int8_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int16_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int32_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int64_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_int1_ty() const noexcept -> llvm::IntegerType*;
    // Pointer-width integer type, per the module's own target data layout.
    [[nodiscard]] auto get_usize_ty() const noexcept -> llvm::IntegerType*;
    [[nodiscard]] auto get_float_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_double_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_void_ty() const noexcept -> llvm::Type*;
    [[nodiscard]] auto get_ptr_ty() const noexcept -> llvm::PointerType*;

    // The LLVM element index of ghoti field `field` in `struct_ty`, past any inserted padding
    [[nodiscard]] auto struct_field_index(llvm::Type* struct_ty, u32 field) const -> u32;
    // A struct (or array of them) whose `@[align(n)]` fields raise its alignment past LLVM's
    [[nodiscard]] auto explicit_alignment_of(const sema::type& type) -> stdx::option<u64>;

  private:
    using type_cache_t = ankerl::unordered_dense::map<const sema::type*, llvm::Type*>;
    using aggregate_identity_cache_t =
        ankerl::unordered_dense::map<sema::types::key_t, llvm::Type*>;

    struct padded_layout {
        std::vector<u32> field_indices;
        u64              alignment;
    };

  private:
    auto translate_slice(const sema::types::slice& s) -> llvm::Type*;
    auto translate_array(const sema::types::array& a) -> llvm::Type*;
    // An array element or struct field: a zero-sized `void` / `type` one is an empty struct
    auto translate_slot(const sema::type& slot) -> llvm::Type*;
    auto translate_struct(const sema::types::struct_t& s, const sema::type& original)
        -> llvm::Type*;
    auto set_struct_body(llvm::StructType*            struct_ty,
                         const sema::types::struct_t& s,
                         gsl::span<llvm::Type* const> element_types) -> void;
    // Lays `element_types` out at the offsets sema gave them (`wanted_alignments`, `@[align(n)]`
    // included) wherever LLVM's own alignment would put them elsewhere
    auto set_padded_body(llvm::StructType*            struct_ty,
                         gsl::span<llvm::Type* const> element_types,
                         gsl::span<const u64>         wanted_alignments,
                         bool                         is_packed) -> void;
    auto translate_union(const sema::types::union_t& u, const sema::type& original) -> llvm::Type*;
    auto translate_enum(const sema::types::enum_t& e) -> llvm::Type*;
    auto translate_closure(const sema::types::closure_t& c, const sema::type& original)
        -> llvm::Type*;

  private:
    llvm::LLVMContext&         context_;
    llvm::Module&              module_;
    type_cache_t               struct_cache_;
    type_cache_t               union_cache_;
    type_cache_t               closure_cache_;
    aggregate_identity_cache_t struct_identity_cache_;
    aggregate_identity_cache_t union_identity_cache_;

    ankerl::unordered_dense::map<const llvm::Type*, padded_layout> padded_structs_;
};

} // namespace ghoti::codegen
