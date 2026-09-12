#pragma once

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
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

  private:
    using type_cache_t = ankerl::unordered_dense::map<const sema::type*, llvm::Type*>;
    // A `const`/`mut` sibling pair of the same aggregate (`type_pool::with_const`) are two
    // distinct `sema::type` objects that share the same underlying `struct_t`/`union_t`/
    // `closure_t` data (a shallow copy of the same field/member spans) - so caching by `&type`
    // alone would translate each sibling to its own, separately-`llvm::StructType::create`d named
    // LLVM type. Two structurally identical but distinct named types then trip LLVM's own
    // "calling a function with bad signature" check the moment one sibling's value crosses into a
    // context expecting the other (e.g. a `var` global's value passed where a by-value `self`
    // param - conventionally resolved as the const sibling - is expected). A sibling pair's own
    // `key_t` differs *only* in its mutability bit (same `kind_`/`markers_`, including a generic
    // instantiation's own per-instantiation discriminator) - so a `key_t` with that bit normalized
    // is a mutability-invariant identity to fall back to on a cache miss, still distinguishing
    // separate instantiations of the same generic aggregate from one another (unlike
    // `symbol_table_idx`, which a generic template's every instantiation shares).
    using aggregate_identity_cache_t =
        ankerl::unordered_dense::map<sema::types::key_t, llvm::Type*>;

  private:
    auto translate_slice(const sema::types::slice& s) -> llvm::Type*;
    auto translate_array(const sema::types::array& a) -> llvm::Type*;
    auto translate_struct(const sema::types::struct_t& s, const sema::type& original)
        -> llvm::Type*;
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
};

} // namespace ghoti::codegen
