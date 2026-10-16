#include "compiler/codegen/c_abi.hh"

#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/Casting.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/codegen/aggregate_copy.hh"

namespace ghoti::codegen {

namespace {

enum class abi_model : u8 {
    NONE,
    WIN64,
    SYSV_X86_64,
    AAPCS64,
    DARWIN_AARCH64,
};

[[nodiscard]] auto model_for(const llvm::Triple& triple, llvm::CallingConv::ID conv) -> abi_model {
    if (triple.getArch() == llvm::Triple::x86_64) {
        if (conv == llvm::CallingConv::Win64) { return abi_model::WIN64; }
        if (conv == llvm::CallingConv::X86_64_SysV) { return abi_model::SYSV_X86_64; }
        return triple.isOSWindows() || triple.isUEFI() ? abi_model::WIN64 : abi_model::SYSV_X86_64;
    }
    if (triple.isAArch64()) {
        return triple.isOSDarwin() ? abi_model::DARWIN_AARCH64 : abi_model::AAPCS64;
    }
    return abi_model::NONE;
}

enum class pass_kind : u8 {
    DIRECT,   // unchanged
    COERCE,   // the bytes travel as `parts`, each its own parameter
    INDIRECT, // a pointer to a copy the caller owns, or an `sret` slot for a result
    BYVAL,    // a pointer the callee treats as its own stack copy
};

struct value_plan {
    pass_kind                kind{pass_kind::DIRECT};
    llvm::Type*              original{nullptr};
    std::vector<llvm::Type*> parts;
    u64                      stack_align{0}; // an AAPCS64 HFA's stack slot alignment, if any
};

struct signature_plan {
    value_plan              ret;
    std::vector<value_plan> params;
    llvm::FunctionType*     lowered{nullptr};
};

// One scalar inside an aggregate and its byte offset (union members overlap)
struct leaf {
    u64         offset;
    llvm::Type* type;
};

// The SysV x86-64 class of one eightbyte
enum class sysv_class : u8 {
    NONE,
    INTEGER,
    SSE,
    SSEUP,
    X87,
    X87UP,
    MEMORY,
};

using sysv_classes = std::array<sysv_class, 2>;

// How two classes sharing an eightbyte combine, in clang's order: INTEGER beats x87
[[nodiscard]] constexpr auto merge(sysv_class accum, sysv_class field) -> sysv_class {
    using enum sysv_class;
    if (accum == field || field == NONE) { return accum; }
    if (field == MEMORY) { return MEMORY; }
    if (accum == NONE) { return field; }
    if (accum == INTEGER || field == INTEGER) { return INTEGER; }
    if (field == X87 || field == X87UP || accum == X87 || accum == X87UP) { return MEMORY; }
    return SSE;
}

class classifier {
  public:
    classifier(llvm::Module& module, const union_member_map& unions)
        : layout_{module.getDataLayout()}, context_{module.getContext()},
          triple_{module.getTargetTriple()}, unions_{unions} {}

    // How `fn_ty` crosses the C ABI, or none when nothing about it changes
    [[nodiscard]] auto plan(llvm::FunctionType*          fn_ty,
                            llvm::CallingConv::ID        conv,
                            std::span<llvm::Type* const> variadic_args = {}) const
        -> stdx::option<signature_plan> {
        const auto model{model_for(triple_, conv)};
        if (model == abi_model::NONE) { return stdx::none; }

        signature_plan  result;
        register_budget budget;
        result.ret = plan_return(model, fn_ty->getReturnType());
        if (result.ret.kind == pass_kind::INDIRECT && budget.integer > 0) { --budget.integer; }

        bool       changed{result.ret.kind != pass_kind::DIRECT};
        const auto add_param{[&](llvm::Type* ty) {
            auto& param{result.params.emplace_back(plan_param(model, ty, budget))};
            changed = changed || param.kind != pass_kind::DIRECT;
        }};
        for (auto* param : fn_ty->params()) { add_param(param); }
        for (auto* extra : variadic_args) { add_param(extra); }
        if (!changed) { return stdx::none; }

        result.lowered = lowered_type(result, fn_ty->isVarArg(), fn_ty->getNumParams());
        return result;
    }

  private:
    struct register_budget {
        u32 integer{6};
        u32 sse{8};
    };

    [[nodiscard]] auto alloc_size(llvm::Type* ty) const -> u64 {
        return layout_.getTypeAllocSize(ty).getFixedValue();
    }

    [[nodiscard]] static auto is_by_value_aggregate(llvm::Type* ty) -> bool {
        return (ty->isStructTy() || ty->isArrayTy()) && ty->isSized();
    }

    [[nodiscard]] auto int_of_bytes(u64 bytes) const -> llvm::Type* {
        return llvm::IntegerType::get(context_, static_cast<u32>(bytes * 8));
    }

    [[nodiscard]] auto union_members_of(llvm::Type* ty) const -> const std::vector<llvm::Type*>* {
        const auto it{unions_.find(ty)};
        return it == unions_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] static auto coerced(llvm::Type* original, std::vector<llvm::Type*> parts)
        -> value_plan {
        return {.kind = pass_kind::COERCE, .original = original, .parts = std::move(parts)};
    }

    [[nodiscard]] static auto direct(llvm::Type* original) -> value_plan {
        return {.kind = pass_kind::DIRECT, .original = original, .parts = {}};
    }

    [[nodiscard]] static auto memory(llvm::Type* original, pass_kind kind) -> value_plan {
        return {.kind = kind, .original = original, .parts = {}};
    }

    [[nodiscard]] auto plan_return(abi_model model, llvm::Type* ty) const -> value_plan {
        if (!is_by_value_aggregate(ty) || alloc_size(ty) == 0) { return direct(ty); }
        switch (model) {
        case abi_model::WIN64:          return win64(ty);
        case abi_model::SYSV_X86_64:    return sysv_return(ty);
        case abi_model::AAPCS64:
        case abi_model::DARWIN_AARCH64: return aarch64_return(ty);
        case abi_model::NONE:           break;
        }
        return direct(ty);
    }

    [[nodiscard]] auto plan_param(abi_model model, llvm::Type* ty, register_budget& budget) const
        -> value_plan {
        if (!is_by_value_aggregate(ty) || alloc_size(ty) == 0) {
            if (model == abi_model::SYSV_X86_64) { spend_scalar(ty, budget); }
            return direct(ty);
        }
        switch (model) {
        case abi_model::WIN64:          return win64(ty);
        case abi_model::SYSV_X86_64:    return sysv_param(ty, budget);
        case abi_model::AAPCS64:        return aarch64_param(ty, false);
        case abi_model::DARWIN_AARCH64: return aarch64_param(ty, true);
        case abi_model::NONE:           break;
        }
        return direct(ty);
    }

    // Win64: 1, 2, 4, and 8 byte aggregates travel as an integer, anything else by reference
    [[nodiscard]] auto win64(llvm::Type* ty) const -> value_plan {
        const auto size{alloc_size(ty)};
        if (size == 1 || size == 2 || size == 4 || size == 8) {
            return coerced(ty, {int_of_bytes(size)});
        }
        return memory(ty, pass_kind::INDIRECT);
    }

    // SysV x86-64 registers available for scalar arguments, which still use them up
    static auto spend_scalar(llvm::Type* ty, register_budget& budget) -> void {
        if (ty->isIntegerTy(128)) {
            budget.integer -= std::min(budget.integer, 2U);
        } else if (ty->isIntegerTy() || ty->isPointerTy()) {
            budget.integer -= std::min(budget.integer, 1U);
        } else if (ty->isFloatingPointTy() && !ty->isX86_FP80Ty()) {
            budget.sse -= std::min(budget.sse, 1U);
        } else if (ty->isVectorTy()) {
            budget.sse -= std::min(budget.sse, 1U);
        }
    }

    [[nodiscard]] auto sysv_return(llvm::Type* ty) const -> value_plan {
        const auto classes{sysv_classify(ty)};
        if (!classes) { return memory(ty, pass_kind::INDIRECT); }
        // An x87 value comes back in st0
        if ((*classes)[0] == sysv_class::X87) {
            return coerced(ty, {llvm::Type::getX86_FP80Ty(context_)});
        }
        auto parts{sysv_parts(ty, *classes)};
        if (parts.empty()) { return direct(ty); }
        if (parts.size() == 1) { return coerced(ty, std::move(parts)); }
        return coerced(ty, {llvm::StructType::get(context_, parts)});
    }

    // An aggregate goes in registers only when all of its eight bytes fit, otherwise on the stack
    [[nodiscard]] auto sysv_param(llvm::Type* ty, register_budget& budget) const -> value_plan {
        const auto classes{sysv_classify(ty)};
        if (!classes) { return memory(ty, pass_kind::BYVAL); }
        const auto count{[&](sysv_class wanted) {
            return static_cast<u32>(std::ranges::count(*classes, wanted));
        }};
        if (count(sysv_class::X87) + count(sysv_class::X87UP) > 0) {
            return memory(ty, pass_kind::BYVAL);
        }
        const auto int_regs{count(sysv_class::INTEGER)};
        const auto sse_regs{count(sysv_class::SSE)};
        if (int_regs > budget.integer || sse_regs > budget.sse) {
            return memory(ty, pass_kind::BYVAL);
        }
        auto parts{sysv_parts(ty, *classes)};
        if (parts.empty()) { return direct(ty); }
        budget.integer -= int_regs;
        budget.sse -= sse_regs;
        return coerced(ty, std::move(parts));
    }

    auto collect_leaves(llvm::Type* ty, u64 base, std::vector<leaf>& out) const -> void {
        if (const auto* members{union_members_of(ty)}) {
            for (auto* member : *members) { collect_leaves(member, base, out); }
        } else if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            const auto* st_layout{layout_.getStructLayout(st)};
            for (u32 i{0}; i < st->getNumElements(); ++i) {
                collect_leaves(st->getElementType(i), base + st_layout->getElementOffset(i), out);
            }
        } else if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            const auto stride{alloc_size(arr->getElementType())};
            for (u64 i{0}; i < arr->getNumElements(); ++i) {
                collect_leaves(arr->getElementType(), base + (i * stride), out);
            }
        } else {
            out.emplace_back(base, ty);
        }
    }

    [[nodiscard]] auto leaves_of(llvm::Type* ty) const -> std::vector<leaf> {
        std::vector<leaf> leaves;
        collect_leaves(ty, 0, leaves);
        return leaves;
    }

    // The classes of the eightbyte a scalar starts in and of the one after it
    [[nodiscard]] auto sysv_leaf_classes(llvm::Type* ty) const -> sysv_classes {
        using enum sysv_class;
        const auto size{alloc_size(ty)};
        if (ty->isX86_FP80Ty()) { return {X87, X87UP}; }
        if (ty->isFP128Ty()) { return {SSE, SSEUP}; }
        if (ty->isIntegerTy() && size == 16) { return {INTEGER, INTEGER}; }
        if (ty->isIntegerTy() || ty->isPointerTy()) { return {size <= 8 ? INTEGER : MEMORY, NONE}; }
        if (ty->isFloatingPointTy()) { return {SSE, NONE}; }
        if (auto* vec{llvm::dyn_cast<llvm::FixedVectorType>(ty)}) {
            if (size <= 4) { return {INTEGER, NONE}; }
            if (size == 8) {
                const bool one_i64{vec->getNumElements() == 1 &&
                                   vec->getElementType()->isIntegerTy(64)};
                return {one_i64 ? INTEGER : SSE, NONE};
            }
            if (size == 16) { return {SSE, SSEUP}; }
        }
        return {MEMORY, NONE};
    }

    // The SysV classification of an aggregate of at most 16 bytes, or none for memory
    [[nodiscard]] auto sysv_classify(llvm::Type* ty) const -> stdx::option<sysv_classes> {
        using enum sysv_class;
        const auto size{alloc_size(ty)};
        if (size > 16) { return stdx::none; }

        sysv_classes classes{NONE, NONE};
        for (const auto& [offset, leaf_ty] : leaves_of(ty)) {
            const auto leaf_size{alloc_size(leaf_ty)};
            const bool misaligned{offset % layout_.getABITypeAlign(leaf_ty).value() != 0};
            const bool straddles{leaf_size <= 8 && (offset / 8) != ((offset + leaf_size - 1) / 8)};
            if (misaligned || straddles) { return stdx::none; }

            const auto [first, second]{sysv_leaf_classes(leaf_ty)};
            const auto eightbyte{offset / 8};
            classes[eightbyte] = merge(classes[eightbyte], first);
            if (second != NONE) { classes[eightbyte + 1] = merge(classes[eightbyte + 1], second); }
            if (classes[0] == MEMORY || classes[1] == MEMORY) { return stdx::none; }
        }

        auto& [lo, hi]{classes};
        // Darwin predates ABI revision 0.98, so a stray X87UP still travels as SSE there
        if (hi == X87UP && lo != X87) {
            if (!triple_.isOSDarwin()) { return stdx::none; }
            hi = SSE;
        }
        if (hi == SSEUP && lo != SSE) { hi = SSE; }
        return classes;
    }

    // One register-sized part per classified eightbyte, typed the way clang picks them
    [[nodiscard]] auto sysv_parts(llvm::Type* ty, const sysv_classes& classes) const
        -> std::vector<llvm::Type*> {
        if (classes[0] == sysv_class::SSE && classes[1] == sysv_class::SSEUP) {
            return {byte_vector_type(ty)};
        }
        const auto               size{alloc_size(ty)};
        const auto               leaves{leaves_of(ty)};
        std::vector<llvm::Type*> parts;
        for (u64 eightbyte{0}; eightbyte * 8 < size; ++eightbyte) {
            const auto offset{eightbyte * 8};
            if (classes[eightbyte] == sysv_class::INTEGER) {
                auto* part{integer_type_at(ty, offset, offset, size, leaves)};
                parts.emplace_back(part);
                // An `i128` covers both eightbytes
                if (alloc_size(part) == 16) { break; }
            } else if (classes[eightbyte] == sysv_class::SSE) {
                parts.emplace_back(sse_type_at(ty, offset, size - offset));
            }
        }
        return parts;
    }

    // clang's `BitsContainNoUserData`: no scalar overlaps bytes [begin, end)
    [[nodiscard]] auto is_padding(const std::vector<leaf>& leaves, u64 begin, u64 end) const
        -> bool {
        return std::ranges::none_of(leaves, [&](const leaf& item) {
            return item.offset < end && item.offset + alloc_size(item.type) > begin;
        });
    }

    // clang's `GetINTEGERTypeAtOffset`: the scalar at `offset` when nothing else shares its
    // eightbyte, otherwise an integer as wide as what is left of the eightbyte
    [[nodiscard]] auto integer_type_at(llvm::Type*              ty,
                                       u64                      offset,
                                       u64                      source_offset,
                                       u64                      source_size,
                                       const std::vector<leaf>& leaves) const -> llvm::Type* {
        if (offset == 0) {
            if (ty->isPointerTy() || ty->isIntegerTy(64) || ty->isIntegerTy(128)) { return ty; }
            if (ty->isIntegerTy(8) || ty->isIntegerTy(16) || ty->isIntegerTy(32)) {
                const auto used{source_offset + (ty->getIntegerBitWidth() / 8)};
                if (is_padding(leaves, used, source_offset + 8)) { return ty; }
            }
        }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            const auto* st_layout{layout_.getStructLayout(st)};
            if (st->getNumElements() > 0 && offset < st_layout->getSizeInBytes()) {
                const auto index{st_layout->getElementContainingOffset(offset)};
                return integer_type_at(st->getElementType(index),
                                       offset - st_layout->getElementOffset(index),
                                       source_offset,
                                       source_size,
                                       leaves);
            }
        }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            const auto stride{alloc_size(arr->getElementType())};
            return integer_type_at(
                arr->getElementType(), offset % stride, source_offset, source_size, leaves);
        }
        return int_of_bytes(std::min<u64>(source_size - source_offset, 8));
    }

    // clang's `getFPTypeAtOffset`: the floating-point scalar starting exactly at `offset`
    [[nodiscard]] auto fp_type_at(llvm::Type* ty, u64 offset) const -> llvm::Type* {
        if (offset == 0 && ty->isFloatingPointTy()) { return ty; }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            const auto* st_layout{layout_.getStructLayout(st)};
            if (st->getNumElements() == 0 || offset >= st_layout->getSizeInBytes()) {
                return nullptr;
            }
            const auto index{st_layout->getElementContainingOffset(offset)};
            return fp_type_at(st->getElementType(index),
                              offset - st_layout->getElementOffset(index));
        }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            return fp_type_at(arr->getElementType(), offset % alloc_size(arr->getElementType()));
        }
        return nullptr;
    }

    // clang's `GetSSETypeAtOffset`: float pairs share a `<2 x float>`, halves pack into a
    // vector, and anything else is a `double`
    [[nodiscard]] auto sse_type_at(llvm::Type* ty, u64 offset, u64 source_size) const
        -> llvm::Type* {
        auto* f64{llvm::Type::getDoubleTy(context_)};
        auto* first{fp_type_at(ty, offset)};
        if (!first || first->isDoubleTy()) { return f64; }

        const auto first_size{alloc_size(first)};
        const bool first_half{first->is16bitFPTy()};
        auto*      second{source_size > first_size ? fp_type_at(ty, offset + first_size) : nullptr};
        if (!second && first_half && source_size > 4) { second = fp_type_at(ty, offset + 4); }
        if (!second) { return first; }

        if (first->isFloatTy() && second->isFloatTy()) {
            return llvm::FixedVectorType::get(first, 2);
        }
        if (first_half && second->is16bitFPTy()) {
            const bool four{source_size > 4 && fp_type_at(ty, offset + 4)};
            return llvm::FixedVectorType::get(first, four ? 4 : 2);
        }
        if (first_half || second->is16bitFPTy()) {
            return llvm::FixedVectorType::get(llvm::Type::getHalfTy(context_), 4);
        }
        return f64;
    }

    // clang's `GetByteVectorType` for an SSE+SSEUP pair: a lone vector or `f128` inside
    // single-member wrappers passes as itself, anything else as `<2 x double>`
    [[nodiscard]] auto byte_vector_type(llvm::Type* ty) const -> llvm::Type* {
        auto* inner{ty};
        while (true) {
            if (const auto* members{union_members_of(inner)}) {
                if (members->size() != 1) { break; }
                inner = members->front();
            } else if (auto* st{llvm::dyn_cast<llvm::StructType>(inner)}) {
                const auto sized{[&](llvm::Type* element) { return alloc_size(element) > 0; }};
                const auto elements{st->elements()};
                if (std::ranges::count_if(elements, sized) != 1) { break; }
                inner = *std::ranges::find_if(elements, sized);
            } else if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(inner)};
                       arr && arr->getNumElements() == 1) {
                inner = arr->getElementType();
            } else {
                break;
            }
        }
        if (alloc_size(inner) == alloc_size(ty) && (inner->isVectorTy() || inner->isFP128Ty())) {
            return inner;
        }
        return llvm::FixedVectorType::get(llvm::Type::getDoubleTy(context_), 2);
    }

    struct hfa_shape {
        llvm::Type* base;
        u64         members;
    };

    // Floating-point scalars, and 8 or 16 byte vectors, can make up an AAPCS64 HFA
    [[nodiscard]] auto is_hfa_base(llvm::Type* ty) const -> bool {
        if (ty->isVectorTy()) { return alloc_size(ty) == 8 || alloc_size(ty) == 16; }
        return ty->isFloatingPointTy() && !ty->isX86_FP80Ty() && !ty->isPPC_FP128Ty();
    }

    // clang's `isHomogeneousAggregate` count: struct members add up, union members take the
    // largest, and every scalar must match `base` in size and vector-ness
    [[nodiscard]] auto hfa_members(llvm::Type* ty, llvm::Type*& base) const -> stdx::option<u64> {
        if (const auto* members{union_members_of(ty)}) {
            u64 most{0};
            for (auto* member : *members) {
                const auto count{hfa_members(member, base)};
                if (!count) { return stdx::none; }
                most = std::max(most, *count);
            }
            return most;
        }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            u64 total{0};
            for (auto* element : st->elements()) {
                const auto count{hfa_members(element, base)};
                if (!count) { return stdx::none; }
                total += *count;
            }
            return total;
        }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            if (arr->getNumElements() == 0) { return stdx::none; }
            const auto count{hfa_members(arr->getElementType(), base)};
            if (!count) { return stdx::none; }
            return *count * arr->getNumElements();
        }
        if (!is_hfa_base(ty)) { return stdx::none; }
        if (!base) {
            base = ty;
        } else if (base->isVectorTy() != ty->isVectorTy() || alloc_size(base) != alloc_size(ty)) {
            return stdx::none;
        }
        return 1;
    }

    // An AAPCS64 homogeneous floating-point aggregate: one to four members of a single base
    [[nodiscard]] auto homogeneous_member(llvm::Type* ty) const -> stdx::option<hfa_shape> {
        llvm::Type* base{nullptr};
        const auto  count{hfa_members(ty, base)};
        if (!count || !base || *count == 0 || *count > 4) { return stdx::none; }
        if (alloc_size(base) * *count != alloc_size(ty)) { return stdx::none; }
        return hfa_shape{.base = base, .members = *count};
    }

    [[nodiscard]] auto aarch64_pair(llvm::Type* ty) const -> llvm::Type* {
        if (layout_.getABITypeAlign(ty).value() >= 16) {
            return llvm::IntegerType::get(context_, 128);
        }
        return llvm::ArrayType::get(llvm::Type::getInt64Ty(context_), 2);
    }

    [[nodiscard]] auto aarch64_param(llvm::Type* ty, bool darwin) const -> value_plan {
        if (const auto hfa{homogeneous_member(ty)}) {
            auto plan{coerced(ty, {llvm::ArrayType::get(hfa->base, hfa->members)})};
            // Linux keeps an HFA's stack slot at 8 bytes, or 16 for a 16-byte aligned one
            if (!darwin) { plan.stack_align = layout_.getABITypeAlign(ty).value() >= 16 ? 16 : 8; }
            return plan;
        }
        const auto size{alloc_size(ty)};
        if (size <= 8) { return coerced(ty, {llvm::Type::getInt64Ty(context_)}); }
        if (size <= 16) { return coerced(ty, {aarch64_pair(ty)}); }
        return memory(ty, pass_kind::INDIRECT);
    }

    [[nodiscard]] auto aarch64_return(llvm::Type* ty) const -> value_plan {
        if (homogeneous_member(ty)) { return direct(ty); }
        const auto size{alloc_size(ty)};
        if (size <= 8) { return coerced(ty, {int_of_bytes(size)}); }
        if (size <= 16) { return coerced(ty, {aarch64_pair(ty)}); }
        return memory(ty, pass_kind::INDIRECT);
    }

    [[nodiscard]] auto lowered_type(const signature_plan& plan, bool is_var_arg, u32 named) const
        -> llvm::FunctionType* {
        std::vector<llvm::Type*> params;
        auto*                    ptr_ty{llvm::PointerType::get(context_, 0)};
        llvm::Type*              ret_ty{plan.ret.original};
        if (plan.ret.kind == pass_kind::INDIRECT) {
            params.emplace_back(ptr_ty);
            ret_ty = llvm::Type::getVoidTy(context_);
        } else if (plan.ret.kind == pass_kind::COERCE) {
            ret_ty = plan.ret.parts.front();
        }
        for (u32 i{0}; i < named; ++i) {
            const auto& param{plan.params[i]};
            switch (param.kind) {
            case pass_kind::DIRECT: params.emplace_back(param.original); break;
            case pass_kind::COERCE:
                params.insert(params.end(), param.parts.begin(), param.parts.end());
                break;
            case pass_kind::INDIRECT:
            case pass_kind::BYVAL:    params.emplace_back(ptr_ty); break;
            }
        }
        return llvm::FunctionType::get(ret_ty, params, is_var_arg);
    }

  private:
    const llvm::DataLayout& layout_;
    llvm::LLVMContext&      context_;
    llvm::Triple            triple_;
    const union_member_map& unions_;
};

// Moves values between their ghoti and C ABI shapes through stack memory, as clang does, and
// leaves the round trips for later passes to fold
class abi_rewriter {
  public:
    abi_rewriter(llvm::Module& module, const union_member_map& unions)
        : module_{module}, layout_{module.getDataLayout()}, context_{module.getContext()},
          classifier_{module, unions}, copier_{module} {}

    auto run() -> void {
        rewrite_signatures();
        rewrite_calls();
    }

  private:
    [[nodiscard]] static auto is_rewritable(const llvm::Function& fn) -> bool {
        return !fn.isIntrinsic() && !fn.hasFnAttribute(llvm::Attribute::Naked);
    }

    [[nodiscard]] auto entry_alloca(llvm::Function& fn, llvm::Type* ty, llvm::Align align)
        -> llvm::AllocaInst* {
        llvm::IRBuilder<> builder{&*fn.getEntryBlock().getFirstInsertionPt()};
        auto*             slot{builder.CreateAlloca(ty, nullptr, "abi.tmp")};
        slot->setAlignment(std::max(align, layout_.getPrefTypeAlign(ty)));
        return slot;
    }

    // A scratch slot big and aligned enough for both shapes of a coerced value
    [[nodiscard]] auto coercion_slot(llvm::Function& fn, const value_plan& plan)
        -> std::pair<llvm::AllocaInst*, llvm::StructType*> {
        auto*      parts_ty{llvm::StructType::get(context_, plan.parts)};
        const auto bytes{std::max(layout_.getTypeAllocSize(plan.original).getFixedValue(),
                                  layout_.getTypeAllocSize(parts_ty).getFixedValue())};
        const auto align{
            std::max(layout_.getABITypeAlign(plan.original), layout_.getABITypeAlign(parts_ty))};
        auto* slot{
            entry_alloca(fn, llvm::ArrayType::get(llvm::Type::getInt8Ty(context_), bytes), align)};
        return {slot, parts_ty};
    }

    [[nodiscard]] auto split(llvm::IRBuilder<>& builder, llvm::Value* value, const value_plan& plan)
        -> std::vector<llvm::Value*> {
        auto [slot, parts_ty]{coercion_slot(*builder.GetInsertBlock()->getParent(), plan)};
        builder.CreateStore(value, slot);
        std::vector<llvm::Value*> parts;
        for (u32 i{0}; i < plan.parts.size(); ++i) {
            auto* at{builder.CreateStructGEP(parts_ty, slot, i)};
            parts.emplace_back(builder.CreateLoad(plan.parts[i], at, "abi.part"));
        }
        return parts;
    }

    [[nodiscard]] auto join(llvm::IRBuilder<>&      builder,
                            std::span<llvm::Value*> parts,
                            const value_plan&       plan) -> llvm::Value* {
        auto [slot, parts_ty]{coercion_slot(*builder.GetInsertBlock()->getParent(), plan)};
        for (usize i{0}; i < parts.size(); ++i) {
            builder.CreateStore(parts[i],
                                builder.CreateStructGEP(parts_ty, slot, static_cast<u32>(i)));
        }
        return builder.CreateLoad(plan.original, slot, "abi.joined");
    }

    [[nodiscard]] auto memory_align(llvm::Type* ty) const -> llvm::Align {
        return std::max(layout_.getABITypeAlign(ty), llvm::Align{8});
    }

    [[nodiscard]] auto memory_param_attrs(const value_plan& plan) -> llvm::AttributeSet {
        llvm::AttrBuilder attrs{context_};
        if (plan.kind == pass_kind::BYVAL) {
            attrs.addByValAttr(plan.original);
            attrs.addAlignmentAttr(memory_align(plan.original));
        }
        if (plan.stack_align > 0) { attrs.addStackAlignmentAttr(llvm::Align{plan.stack_align}); }
        return llvm::AttributeSet::get(context_, attrs);
    }

    // The lowered attribute list: scalar attributes follow their parameter to its new position
    [[nodiscard]] auto lowered_attributes(const llvm::AttributeList& old,
                                          const signature_plan&      plan,
                                          u32 arg_count) -> llvm::AttributeList {
        std::vector<llvm::AttributeSet> params;
        if (plan.ret.kind == pass_kind::INDIRECT) {
            llvm::AttrBuilder sret{context_};
            sret.addStructRetAttr(plan.ret.original);
            params.emplace_back(llvm::AttributeSet::get(context_, sret));
        }
        for (u32 i{0}; i < arg_count; ++i) {
            const auto& param{plan.params[i]};
            switch (param.kind) {
            case pass_kind::DIRECT: params.emplace_back(old.getParamAttrs(i)); break;
            case pass_kind::COERCE:
                for (usize p{0}; p < param.parts.size(); ++p) {
                    params.emplace_back(p == 0 ? memory_param_attrs(param) : llvm::AttributeSet{});
                }
                break;
            case pass_kind::INDIRECT:
            case pass_kind::BYVAL:    params.emplace_back(memory_param_attrs(param)); break;
            }
        }
        const auto ret_attrs{plan.ret.kind == pass_kind::DIRECT ? old.getRetAttrs()
                                                                : llvm::AttributeSet{}};
        return llvm::AttributeList::get(context_, old.getFnAttrs(), ret_attrs, params);
    }

    // Rebuilds each old parameter from its lowered form at the top of the body
    auto rebind_params(llvm::Function& old_fn, llvm::Function& new_fn, const signature_plan& plan)
        -> void {
        llvm::IRBuilder<> builder{&*new_fn.getEntryBlock().getFirstInsertionPt()};
        u32               next{plan.ret.kind == pass_kind::INDIRECT ? 1U : 0U};
        for (auto& old_arg : old_fn.args()) {
            const auto& param{plan.params[old_arg.getArgNo()]};
            switch (param.kind) {
            case pass_kind::DIRECT: {
                auto* new_arg{new_fn.getArg(next++)};
                new_arg->takeName(&old_arg);
                old_arg.replaceAllUsesWith(new_arg);
                break;
            }
            case pass_kind::COERCE: {
                std::vector<llvm::Value*> parts;
                for (usize p{0}; p < param.parts.size(); ++p) {
                    parts.emplace_back(new_fn.getArg(next++));
                }
                old_arg.replaceAllUsesWith(join(builder, parts, param));
                break;
            }
            case pass_kind::INDIRECT:
            case pass_kind::BYVAL:    {
                auto* pointer{new_fn.getArg(next++)};
                pointer->takeName(&old_arg);
                copier_.redirect_indirect_param(old_arg, *pointer, param.original);
                break;
            }
            }
        }
    }

    auto rewrite_returns(llvm::Function& fn, const value_plan& ret) -> void {
        if (ret.kind == pass_kind::INDIRECT) {
            copier_.redirect_returns(fn, fn.getArg(0), ret.original);
            return;
        }
        for (auto& block : fn) {
            auto* old_ret{llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator())};
            if (!old_ret || !old_ret->getReturnValue()) { continue; }
            llvm::IRBuilder<> builder{old_ret};
            auto*             value{old_ret->getReturnValue()};
            builder.CreateRet(split(builder, value, ret).front());
            old_ret->eraseFromParent();
        }
    }

    auto rewrite_signatures() -> void {
        PROFILE_FUNCTION();
        std::vector<std::pair<llvm::Function*, signature_plan>> candidates;
        for (auto& fn : module_) {
            if (!is_rewritable(fn)) { continue; }
            if (auto plan{classifier_.plan(fn.getFunctionType(), fn.getCallingConv())}) {
                candidates.emplace_back(&fn, std::move(*plan));
            }
        }

        for (auto& [old_fn, plan] : candidates) {
            auto* new_fn{llvm::Function::Create(
                plan.lowered, old_fn->getLinkage(), old_fn->getAddressSpace(), "", &module_)};
            new_fn->copyAttributesFrom(old_fn);
            new_fn->setComdat(old_fn->getComdat());
            new_fn->setAttributes(lowered_attributes(
                old_fn->getAttributes(), plan, static_cast<u32>(old_fn->arg_size())));
            new_fn->takeName(old_fn);

            if (!old_fn->isDeclaration()) {
                new_fn->splice(new_fn->end(), old_fn);
                rebind_params(*old_fn, *new_fn, plan);
                if (plan.ret.kind != pass_kind::DIRECT) { rewrite_returns(*new_fn, plan.ret); }
            }
            old_fn->replaceAllUsesWith(new_fn);
            old_fn->eraseFromParent();
        }
    }

    [[nodiscard]] static auto skips_call(const llvm::CallInst& call) -> bool {
        if (call.isInlineAsm()) { return true; }
        const auto* callee{call.getCalledFunction()};
        return callee && !is_rewritable(*callee);
    }

    // The lowered argument list, with an `sret` slot first when the result travels in memory
    auto lower_arguments(llvm::CallInst& call, const signature_plan& plan, llvm::Value* sret_slot)
        -> std::vector<llvm::Value*> {
        auto&                     caller{*call.getFunction()};
        llvm::IRBuilder<>         builder{&call};
        std::vector<llvm::Value*> args;
        if (sret_slot) { args.emplace_back(sret_slot); }
        for (u32 i{0}; i < call.arg_size(); ++i) {
            auto*       arg{call.getArgOperand(i)};
            const auto& param{plan.params[i]};
            switch (param.kind) {
            case pass_kind::DIRECT: args.emplace_back(arg); break;
            case pass_kind::COERCE: {
                const auto parts{split(builder, arg, param)};
                args.insert(args.end(), parts.begin(), parts.end());
                break;
            }
            case pass_kind::INDIRECT:
            case pass_kind::BYVAL:    {
                auto* copy{entry_alloca(caller, param.original, memory_align(param.original))};
                copier_.materialize_argument(arg, copy, param.original, call);
                args.emplace_back(copy);
                break;
            }
            }
        }
        return args;
    }

    auto rewrite_call(llvm::CallInst& call, const signature_plan& plan) -> void {
        auto&        caller{*call.getFunction()};
        llvm::Value* sret_slot{nullptr};
        if (plan.ret.kind == pass_kind::INDIRECT) {
            sret_slot = entry_alloca(caller, plan.ret.original, memory_align(plan.ret.original));
        }
        const auto args{lower_arguments(call, plan, sret_slot)};

        llvm::IRBuilder<> builder{&call};
        auto*             lowered{builder.CreateCall(plan.lowered, call.getCalledOperand(), args)};
        lowered->setCallingConv(call.getCallingConv());
        lowered->setAttributes(lowered_attributes(call.getAttributes(), plan, call.arg_size()));

        builder.SetInsertPoint(call.getNextNode());
        llvm::Value* result{nullptr};
        switch (plan.ret.kind) {
        case pass_kind::DIRECT:
            lowered->takeName(&call);
            result = lowered;
            break;
        case pass_kind::COERCE: {
            std::array<llvm::Value*, 1> parts{lowered};
            result = join(builder, parts, plan.ret);
            break;
        }
        case pass_kind::INDIRECT:
        case pass_kind::BYVAL:
            copier_.redirect_sret_result(call, sret_slot, plan.ret.original);
            break;
        }
        if (result) { call.replaceAllUsesWith(result); }
        std::vector<llvm::Value*> old_args{call.arg_begin(), call.arg_end()};
        std::ranges::sort(old_args);
        old_args.erase(std::ranges::unique(old_args).begin(), old_args.end());
        call.eraseFromParent();
        for (auto* arg : old_args) { aggregate_copier::erase_if_dead(arg); }
    }

    auto rewrite_calls() -> void {
        PROFILE_FUNCTION();
        std::vector<std::pair<llvm::CallInst*, signature_plan>> calls;
        for (auto& fn : module_) {
            for (auto& inst : llvm::instructions(fn)) {
                auto* call{llvm::dyn_cast<llvm::CallInst>(&inst)};
                if (!call || skips_call(*call)) { continue; }
                auto*                    fn_ty{call->getFunctionType()};
                std::vector<llvm::Type*> variadic;
                for (u32 i{fn_ty->getNumParams()}; i < call->arg_size(); ++i) {
                    variadic.emplace_back(call->getArgOperand(i)->getType());
                }
                if (auto plan{classifier_.plan(fn_ty, call->getCallingConv(), variadic)}) {
                    calls.emplace_back(call, std::move(*plan));
                }
            }
        }
        for (auto& [call, plan] : calls) { rewrite_call(*call, plan); }
    }

  private:
    llvm::Module&           module_;
    const llvm::DataLayout& layout_;
    llvm::LLVMContext&      context_;
    classifier              classifier_;
    aggregate_copier        copier_;
};

} // namespace

auto lower_c_abi(llvm::Module& module, const union_member_map& unions) -> void {
    PROFILE_FUNCTION();
    abi_rewriter{module, unions}.run();
}

} // namespace ghoti::codegen
