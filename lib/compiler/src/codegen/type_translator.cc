#include "compiler/codegen/type_translator.hh"

#include <algorithm>
#include <vector>

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <stdx/assert.hh>
#include <stdx/types.hh>

#include "compiler/sema/type.hh"

namespace ghoti::codegen {

auto type_translator::translate(const sema::type& type) -> llvm::Type* {
    switch (type.get_kind()) {
    case sema::type_kind::I32:
    case sema::type_kind::U32:       return get_int32_ty();
    case sema::type_kind::I64:
    case sema::type_kind::U64:
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:     return get_int64_ty();
    case sema::type_kind::U8:        return get_int8_ty();
    case sema::type_kind::BOOL:      return get_int1_ty();
    case sema::type_kind::F32:       return get_float_ty();
    case sema::type_kind::F64:       return get_double_ty();
    case sema::type_kind::VOID_:
    case sema::type_kind::NORETURN:  return get_void_ty();
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:  return get_ptr_ty();
    case sema::type_kind::SLICE:     return translate_slice(type.get_data().as<sema::types::slice>());
    case sema::type_kind::ARRAY:     return translate_array(type.get_data().as<sema::types::array>());
    case sema::type_kind::STRUCT:
        return translate_struct(type.get_data().as<sema::types::struct_t>(), type);
    case sema::type_kind::UNION:
        return translate_union(type.get_data().as<sema::types::union_t>(), type);
    case sema::type_kind::ENUM: return translate_enum(type.get_data().as<sema::types::enum_t>());
    case sema::type_kind::OPAQUE:
    case sema::type_kind::TYPE:
    case sema::type_kind::MODULE:
    case sema::type_kind::LABEL:
    case sema::type_kind::BLOCK:
    case sema::type_kind::MATCH_ARM:
    case sema::type_kind::POISON:
    case sema::type_kind::UNDEFINED:
    case sema::type_kind::AUTO:      return get_void_ty();
    }
    UNREACHABLE("Unhandled type kind in codegen::type_translator");
}

auto type_translator::translate_function_type(const sema::types::function& fn)
    -> llvm::FunctionType* {
    auto*                    ret_ty{translate(fn.return_type)};
    std::vector<llvm::Type*> param_types;
    param_types.reserve(fn.params.size());
    for (const auto* param : fn.params) {
        auto* p_ty{translate(*param)};
        if (!p_ty->isVoidTy()) {
            param_types.emplace_back(p_ty);
        }
    }
    return llvm::FunctionType::get(ret_ty, param_types, fn.is_variadic);
}

auto type_translator::translate_slice_type() -> llvm::StructType* {
    const std::vector<llvm::Type*> elements{get_ptr_ty(), get_int64_ty()};
    return llvm::StructType::get(context_, elements);
}

auto type_translator::get_int8_ty() const noexcept -> llvm::IntegerType* {
    return llvm::Type::getInt8Ty(context_);
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
    return translate_slice_type();
}

auto type_translator::translate_array(const sema::types::array& a) -> llvm::Type* {
    return llvm::ArrayType::get(translate(a.underlying), a.len);
}

auto type_translator::translate_struct(const sema::types::struct_t& s, const sema::type& original)
    -> llvm::Type* {
    if (const auto it{struct_cache_.find(&original)}; it != struct_cache_.end()) {
        return it->second;
    }

    auto* struct_ty{llvm::StructType::create(context_)};
    struct_cache_[&original] = struct_ty;

    std::vector<llvm::Type*> element_types;
    element_types.reserve(s.fields.size());
    for (const auto* field : s.fields) { element_types.emplace_back(translate(*field)); }
    struct_ty->setBody(element_types, s.is_packed);
    return struct_ty;
}

auto type_translator::translate_union(const sema::types::union_t& u, const sema::type& original)
    -> llvm::Type* {
    if (const auto it{union_cache_.find(&original)}; it != union_cache_.end()) {
        return it->second;
    }

    if (u.is_untagged) {
        u64              max_size{0};
        llvm::DataLayout dl;
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
    union_cache_[&original] = union_ty;
    u64 max_size{0};

    llvm::DataLayout dl;
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
    return translate(e.underlying);
}

} // namespace ghoti::codegen
