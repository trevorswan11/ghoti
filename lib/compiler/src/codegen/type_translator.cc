#include "compiler/codegen/type_translator.hh"

#include <algorithm>
#include <vector>

#include <gsl/span>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/MathExtras.h>
#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/codegen/soft_f80.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/sema/type.hh"

namespace ghoti::codegen {

auto type_translator::translate(const sema::type& type) -> llvm::Type* {
    PROFILE_FUNCTION();
    switch (type.get_kind()) {
    case sema::type_kind::INT:   return llvm::IntegerType::get(context_, sema::int_width(type));
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE: return get_usize_ty();
    case sema::type_kind::BOOL:  return get_int1_ty();
    case sema::type_kind::F16:   return llvm::Type::getHalfTy(context_);
    case sema::type_kind::F32:   return get_float_ty();
    case sema::type_kind::F64:   return get_double_ty();
    case sema::type_kind::F80:   return llvm::Type::getX86_FP80Ty(context_);
    case sema::type_kind::F128:  return llvm::Type::getFP128Ty(context_);
    case sema::type_kind::COMPTIME_INT:
        if (type.get_key().get_int_bits() == sema::CHAR_CONSTANT_BITS) {
            return llvm::IntegerType::get(context_, sema::CHAR_CONSTANT_BITS);
        }
        return get_int32_ty();
    case sema::type_kind::COMPTIME_FLOAT: return get_double_ty();
    case sema::type_kind::VOID_:
    case sema::type_kind::NORETURN:       return get_void_ty();
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:      {
        // `&dyn I` / `^dyn I` is a fat pointer: `{ data: ptr, vtable: ptr }`.
        const auto        p{type.get_data().as_opt<sema::types::pointer>()};
        const auto        r{type.get_data().as_opt<sema::types::reference>()};
        const sema::type* referent{p ? &p->underlying : r ? &r->underlying : nullptr};
        if (referent && referent->get_kind() == sema::type_kind::DYN) {
            return translate_dyn_fat_ptr();
        }
        if (sema::is_fat_callable(type)) { return translate_dyn_fat_ptr(); }
        return get_ptr_ty();
    }
    case sema::type_kind::FUNCTION:
        if (sema::is_erased_fn(type)) { return translate_dyn_fat_ptr(); }
        return get_ptr_ty();
    case sema::type_kind::NULLPTR: return get_ptr_ty();
    case sema::type_kind::SLICE:   return translate_slice(type.get_data().as<sema::types::slice>());
    case sema::type_kind::ARRAY:   return translate_array(type.get_data().as<sema::types::array>());
    case sema::type_kind::STRUCT:
        return translate_struct(type.get_data().as<sema::types::struct_t>(), type);
    case sema::type_kind::UNION:
        return translate_union(type.get_data().as<sema::types::union_t>(), type);
    case sema::type_kind::ENUM: return translate_enum(type.get_data().as<sema::types::enum_t>());
    case sema::type_kind::CLOSURE:
        return translate_closure(type.get_data().as<sema::types::closure_t>(), type);
    case sema::type_kind::DYN:       return translate_dyn_fat_ptr();
    case sema::type_kind::OPAQUE:
    case sema::type_kind::INTERFACE:
    case sema::type_kind::TYPE:
    case sema::type_kind::MODULE:
    case sema::type_kind::LABEL:
    case sema::type_kind::BLOCK:
    case sema::type_kind::MATCH_ARM:
    case sema::type_kind::AUTO:      return get_void_ty();
    // These should never survive to codegen; treat reaching here as an internal-compiler error.
    case sema::type_kind::POISON:
    case sema::type_kind::UNDEFINED: UNREACHABLE("POISON/UNDEFINED type reached codegen");
    default:                         UNREACHABLE("Unhandled type kind in codegen::type_translator");
    }
}

auto type_translator::translate_function_type(const sema::types::function& fn)
    -> llvm::FunctionType* {
    PROFILE_FUNCTION();
    auto*                    ret_ty{translate(fn.return_type)};
    std::vector<llvm::Type*> param_types;
    param_types.reserve(fn.params.size());
    for (const auto* param : fn.params) {
        auto* p_ty{translate(*param)};
        if (!p_ty->isVoidTy()) { param_types.emplace_back(p_ty); }
    }
    return llvm::FunctionType::get(ret_ty, param_types, fn.is_variadic);
}

auto type_translator::translate_slice_type() -> llvm::StructType* {
    PROFILE_FUNCTION();
    const std::vector<llvm::Type*> elements{get_ptr_ty(), get_usize_ty()};
    return llvm::StructType::get(context_, elements);
}

auto type_translator::get_int8_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt8Ty(context_);
}

auto type_translator::get_int16_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt16Ty(context_);
}

auto type_translator::get_int32_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt32Ty(context_);
}

auto type_translator::get_int64_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt64Ty(context_);
}

auto type_translator::get_int1_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt1Ty(context_);
}

auto type_translator::get_usize_ty() const noexcept -> llvm::IntegerType* {
    return module_.getDataLayout().getIntPtrType(context_);
}

auto type_translator::get_float_ty() const noexcept -> llvm::Type* {
    return llvm::Type::getFloatTy(context_);
}

auto type_translator::get_double_ty() const noexcept -> llvm::Type* {
    return llvm::Type::getDoubleTy(context_);
}

auto type_translator::get_void_ty() const noexcept -> llvm::Type* {
    return llvm::Type::getVoidTy(context_);
}

auto type_translator::get_ptr_ty() const noexcept -> llvm::PointerType* {
    return llvm::PointerType::get(context_, 0);
}

auto type_translator::translate_slice(const sema::types::slice&) -> llvm::Type* {
    PROFILE_FUNCTION();
    return translate_slice_type();
}

auto type_translator::translate_dyn_fat_ptr() -> llvm::StructType* {
    PROFILE_FUNCTION();
    return llvm::StructType::get(context_, {get_ptr_ty(), get_ptr_ty()});
}

auto type_translator::translate_array(const sema::types::array& a) -> llvm::Type* {
    PROFILE_FUNCTION();
    // A sentinel-terminated array stores one extra element for the terminator
    return llvm::ArrayType::get(translate_slot(a.underlying), a.len + (a.null_terminated ? 1 : 0));
}

auto type_translator::translate_slot(const sema::type& slot) -> llvm::Type* {
    // `translate` maps both to `void`, which isn't a sized LLVM type
    const auto kind{slot.get_kind()};
    if (kind == sema::type_kind::TYPE || kind == sema::type_kind::VOID_) {
        return llvm::StructType::get(context_);
    }
    return translate(slot);
}

auto type_translator::translate_struct(const sema::types::struct_t& s, const sema::type& original)
    -> llvm::Type* {
    PROFILE_FUNCTION();
    if (const auto it{struct_cache_.find(&original)}; it != struct_cache_.end()) {
        return it->second;
    }
    auto mut_invariant_key{original.get_key()};
    mut_invariant_key.set_mut(sema::types::mut::CONSTANT);
    if (const auto it{struct_identity_cache_.find(mut_invariant_key)};
        it != struct_identity_cache_.end()) {
        struct_cache_[&original] = it->second;
        return it->second;
    }

    // A bit-packed `packed struct` is a bare backing integer, not an aggregate.
    if (s.is_bit_packed()) {
        const auto bits{
            sema::packed_backing_bits(s, module_.getDataLayout().getPointerSizeInBits())};
        ASSERT(bits, "a bit-packed struct must have an eligible, in-range layout");
        auto* int_ty{llvm::IntegerType::get(context_, *bits)};
        struct_cache_[&original]                  = int_ty;
        struct_identity_cache_[mut_invariant_key] = int_ty;
        return int_ty;
    }

    auto* struct_ty{llvm::StructType::create(context_)};
    struct_cache_[&original]                  = struct_ty;
    struct_identity_cache_[mut_invariant_key] = struct_ty;

    std::vector<llvm::Type*> element_types;
    element_types.reserve(s.fields.size());
    for (const auto* field : s.fields) { element_types.emplace_back(translate_slot(*field)); }
    set_struct_body(struct_ty, s, element_types);
    return struct_ty;
}

auto type_translator::set_struct_body(llvm::StructType*            struct_ty,
                                      const sema::types::struct_t& s,
                                      gsl::span<llvm::Type* const> element_types) -> void {
    const auto       ptr_size{static_cast<usize>(module_.getDataLayout().getPointerSize())};
    std::vector<u64> wanted;
    wanted.reserve(element_types.size());
    for (usize i{0}; i < element_types.size(); ++i) {
        wanted.emplace_back(s.is_packed ? s.explicit_field_alignment(i)
                                        : gir::const_eval::struct_field_align(s, i, ptr_size));
    }
    set_padded_body(struct_ty, element_types, wanted, s.is_packed);
}

auto type_translator::set_padded_body(llvm::StructType*            struct_ty,
                                      gsl::span<llvm::Type* const> element_types,
                                      gsl::span<const u64>         wanted_alignments,
                                      bool                         is_packed) -> void {
    const auto& dl{module_.getDataLayout()};

    std::vector<llvm::Type*> padded;
    padded_layout            layout{.field_indices = {}, .alignment = 1};
    u64                      end{0};
    bool                     inserted_padding{false};
    const auto               pad_to{[&](u64 offset) {
        if (offset == end) { return; }
        padded.emplace_back(llvm::ArrayType::get(get_int8_ty(), offset - end));
        end              = offset;
        inserted_padding = true;
    }};

    for (usize i{0}; i < element_types.size(); ++i) {
        auto*      elem_ty{element_types[i]};
        const auto natural{is_packed ? 1 : f80_storage_align(module_, elem_ty)};
        const auto wanted{std::max<u64>(natural, wanted_alignments[i])};
        const auto offset{llvm::alignTo(end, wanted)};
        if (offset != llvm::alignTo(end, natural)) { pad_to(offset); }

        layout.field_indices.emplace_back(static_cast<u32>(padded.size()));
        padded.emplace_back(elem_ty);
        end              = offset + dl.getTypeAllocSize(elem_ty).getFixedValue();
        layout.alignment = std::max(layout.alignment, wanted);
    }

    // LLVM rounds the size up to its own natural alignment; any excess becomes trailing padding
    u64 natural_alignment{1};
    for (auto* elem_ty : element_types) {
        if (is_packed) { break; }
        natural_alignment = std::max<u64>(natural_alignment, f80_storage_align(module_, elem_ty));
    }
    const auto total{llvm::alignTo(end, layout.alignment)};
    if (total != llvm::alignTo(end, natural_alignment)) { pad_to(total); }

    if (!inserted_padding) {
        struct_ty->setBody(element_types, is_packed);
        return;
    }
    struct_ty->setBody(padded, is_packed);
    padded_structs_.insert_or_assign(struct_ty, std::move(layout));
}

auto type_translator::struct_field_index(llvm::Type* struct_ty, u32 field) const -> u32 {
    const auto it{padded_structs_.find(struct_ty)};
    return it == padded_structs_.end() ? field : it->second.field_indices[field];
}

auto type_translator::explicit_alignment_of(const sema::type& type) -> stdx::option<u64> {
    if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
        return explicit_alignment_of(arr->underlying);
    }
    if (type.get_kind() != sema::type_kind::STRUCT) { return stdx::none; }
    const auto it{padded_structs_.find(translate(type))};
    if (it == padded_structs_.end()) { return stdx::none; }
    return it->second.alignment;
}

auto type_translator::translate_union(const sema::types::union_t& u, const sema::type& original)
    -> llvm::Type* {
    PROFILE_FUNCTION();
    if (const auto it{union_cache_.find(&original)}; it != union_cache_.end()) {
        return it->second;
    }
    // See the matching comment in `translate_struct`
    auto mut_invariant_key{original.get_key()};
    mut_invariant_key.set_mut(sema::types::mut::CONSTANT);
    const bool is_tagged{!u.is_bit_packed() && !u.is_untagged};
    if (is_tagged) {
        if (const auto it{union_identity_cache_.find(mut_invariant_key)};
            it != union_identity_cache_.end()) {
            union_cache_[&original] = it->second;
            return it->second;
        }
    }

    const auto& dl{module_.getDataLayout()};

    // A bit-packed `packed union` is a bare backing integer wide enough for its largest field.
    if (u.is_bit_packed()) {
        const auto bits{sema::packed_union_backing_bits(u, dl.getPointerSizeInBits())};
        ASSERT(bits, "a bit-packed union must have an eligible, in-range layout");
        auto* int_ty{llvm::IntegerType::get(context_, *bits)};
        union_cache_[&original] = int_ty;
        return int_ty;
    }

    if (u.is_untagged) {
        u64 max_size{0};
        for (const auto* field : u.fields) {
            if (field->get_kind() == sema::type_kind::VOID_) { continue; }
            auto* field_ty{translate(*field)};
            if (field_ty && !field_ty->isVoidTy()) {
                max_size = std::max(max_size, dl.getTypeAllocSize(field_ty).getFixedValue());
            }
        }
        auto* arr_ty{llvm::ArrayType::get(get_int8_ty(), std::max(max_size, u64{1}))};
        union_cache_[&original] = arr_ty;
        return arr_ty;
    }

    auto* union_ty{llvm::StructType::create(context_)};
    union_cache_[&original]                  = union_ty;
    union_identity_cache_[mut_invariant_key] = union_ty;
    u64 max_size{0};

    for (const auto* field : u.fields) {
        if (field->get_kind() == sema::type_kind::VOID_) { continue; }
        auto* field_ty{translate(*field)};
        if (field_ty && !field_ty->isVoidTy()) {
            max_size = std::max(max_size, dl.getTypeAllocSize(field_ty).getFixedValue());
        }
    }

    std::vector<llvm::Type*> elements;
    elements.emplace_back(get_int32_ty());
    if (max_size > 0) { elements.emplace_back(llvm::ArrayType::get(get_int8_ty(), max_size)); }
    union_ty->setBody(elements);
    return union_ty;
}

auto type_translator::translate_enum(const sema::types::enum_t& e) -> llvm::Type* {
    PROFILE_FUNCTION();
    return translate(e.underlying);
}

auto type_translator::translate_closure(const sema::types::closure_t& c, const sema::type& original)
    -> llvm::Type* {
    PROFILE_FUNCTION();
    if (const auto it{closure_cache_.find(&original)}; it != closure_cache_.end()) {
        return it->second;
    }

    auto* closure_ty{llvm::StructType::create(context_)};
    closure_cache_[&original] = closure_ty;

    const auto               ptr_size{static_cast<usize>(module_.getDataLayout().getPointerSize())};
    std::vector<llvm::Type*> element_types;
    std::vector<u64>         wanted;
    element_types.reserve(c.captures.size());
    wanted.reserve(c.captures.size());
    for (const auto& capture : c.captures) {
        element_types.emplace_back(translate(*capture.storage_type));
        wanted.emplace_back(gir::const_eval::type_align_of(*capture.storage_type, ptr_size));
    }
    set_padded_body(closure_ty, element_types, wanted, false);
    return closure_ty;
}

} // namespace ghoti::codegen
