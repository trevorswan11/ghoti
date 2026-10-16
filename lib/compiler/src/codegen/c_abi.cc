#include "compiler/codegen/c_abi.hh"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
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
    X86_32,
    AAPCS32,
    AAPCS32_VFP,
    RISCV,
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
    // fastcall and the MCU ABI pass aggregates in registers, which only cdecl and stdcall avoid
    if (triple.getArch() == llvm::Triple::x86) {
        const bool stack_conv{conv == llvm::CallingConv::C ||
                              conv == llvm::CallingConv::X86_StdCall};
        return stack_conv && !triple.isOSIAMCU() ? abi_model::X86_32 : abi_model::NONE;
    }
    // Apple and Windows ARM use their own variants (APCS, AAPCS16, Windows AAPCS-VFP)
    if (triple.isARM() || triple.isThumb()) {
        if (triple.isOSDarwin() || triple.isOSBinFormatMachO() || triple.isOSWindows()) {
            return abi_model::NONE;
        }
        if (conv == llvm::CallingConv::ARM_AAPCS) { return abi_model::AAPCS32; }
        if (conv == llvm::CallingConv::ARM_AAPCS_VFP) { return abi_model::AAPCS32_VFP; }
        if (conv != llvm::CallingConv::C) { return abi_model::NONE; }
        return triple.isHardFloatABI() ? abi_model::AAPCS32_VFP : abi_model::AAPCS32;
    }
    if (triple.isRISCV() && conv == llvm::CallingConv::C) { return abi_model::RISCV; }
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
    // Where each part sits in the original, when that is not the parts laid out in order. A lone
    // struct part with more offsets than parts is scattered element by element
    std::vector<u64> offsets;
    u64              stack_align{0}; // an HFA's stack slot alignment, if any
    u64              byval_align{0}; // a `byval` copy's alignment, when not the default
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
    classifier(llvm::Module&            module,
               const union_member_map&  unions,
               const padded_struct_map& padded)
        : layout_{module.getDataLayout()}, context_{module.getContext()},
          triple_{module.getTargetTriple()}, unions_{unions}, padded_{padded} {
        // i386 returns small structs in registers only where the OS's ABI says so
        small_struct_in_regs_ = triple_.isOSDarwin() || triple_.isOSWindows() ||
                                triple_.isOSFreeBSD() || triple_.isOSOpenBSD() ||
                                triple_.isOSDragonFly();
        win32_struct_abi_     = triple_.isOSWindows() && !triple_.isOSCygMing();
        if (triple_.isRISCV()) { read_riscv_abi(module); }
    }

    // How `fn_ty` crosses the C ABI, or none when nothing about it changes
    [[nodiscard]] auto plan(llvm::FunctionType*          fn_ty,
                            llvm::CallingConv::ID        conv,
                            std::span<llvm::Type* const> variadic_args = {}) const
        -> stdx::option<signature_plan> {
        const auto model{model_for(triple_, conv)};
        if (model == abi_model::NONE) { return stdx::none; }
        // C varargs, and every argument of a variadic call, follow the base AAPCS
        const auto call_model{
            model == abi_model::AAPCS32_VFP && fn_ty->isVarArg() ? abi_model::AAPCS32 : model};

        signature_plan  result;
        register_budget budget{initial_budget(model)};
        result.ret = plan_return(call_model, fn_ty->getReturnType());
        if (takes_hidden_return(model, result.ret) && budget.integer > 0) { --budget.integer; }

        bool       changed{result.ret.kind != pass_kind::DIRECT};
        const auto add_param{[&](llvm::Type* ty, bool fixed) {
            auto& param{result.params.emplace_back(plan_param(call_model, ty, budget, fixed))};
            changed = changed || param.kind != pass_kind::DIRECT;
        }};
        for (auto* param : fn_ty->params()) { add_param(param, true); }
        for (auto* extra : variadic_args) { add_param(extra, false); }
        if (!changed) { return stdx::none; }

        result.lowered = lowered_type(result, fn_ty->isVarArg(), fn_ty->getNumParams());
        return result;
    }

  private:
    struct register_budget {
        u32 integer{6};
        u32 sse{8};
    };

    // The RISC-V float ABI, from the `target-abi` flag the target machine recorded
    auto read_riscv_abi(const llvm::Module& module) -> void {
        xlen_ = triple_.isRISCV64() ? 64 : 32;
        std::string_view abi{triple_.isOSLinux() ? (xlen_ == 64 ? "lp64d" : "ilp32d")
                                                 : (xlen_ == 64 ? "lp64" : "ilp32")};
        if (const auto* flag{
                llvm::dyn_cast_or_null<llvm::MDString>(module.getModuleFlag("target-abi"))}) {
            abi = flag->getString();
        }
        flen_       = abi.ends_with('d') ? 64 : abi.ends_with('f') ? 32 : 0;
        riscv_e_    = abi.ends_with('e');
        riscv_gprs_ = riscv_e_ ? 6 : 8;
    }

    [[nodiscard]] auto initial_budget(abi_model model) const -> register_budget {
        if (model == abi_model::RISCV) {
            return {.integer = riscv_gprs_, .sse = flen_ > 0 ? 8U : 0U};
        }
        return {};
    }

    // A result in memory takes the first integer register for its address. On RISC-V that is
    // also true of a scalar wider than two registers, which the backend returns indirectly
    [[nodiscard]] auto takes_hidden_return(abi_model model, const value_plan& ret) const -> bool {
        if (ret.kind == pass_kind::INDIRECT) { return true; }
        if (model != abi_model::RISCV || ret.kind != pass_kind::DIRECT) { return false; }
        auto* ty{ret.original};
        return !ty->isVoidTy() && !is_by_value_aggregate(ty) && alloc_size(ty) * 8 > 2 * xlen_;
    }

    [[nodiscard]] auto alloc_size(llvm::Type* ty) const -> u64 {
        return layout_.getTypeAllocSize(ty).getFixedValue();
    }

    // The C alignment of `ty`, which LLVM understates for anything holding an `@[align(n)]` field
    [[nodiscard]] auto abi_align(llvm::Type* ty) const -> u64 {
        u64 align{layout_.getABITypeAlign(ty).value()};
        if (const auto it{padded_.find(ty)}; it != padded_.end()) {
            align = std::max(align, it->second.alignment);
        }
        if (const auto* members{union_members_of(ty)}) {
            for (auto* member : *members) { align = std::max(align, abi_align(member)); }
        } else if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}; st && !st->isPacked()) {
            for (auto* element : st->elements()) { align = std::max(align, abi_align(element)); }
        } else if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            align = std::max(align, abi_align(arr->getElementType()));
        }
        return align;
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

    // A struct's real fields and their offsets, skipping the padding the translator inserted
    [[nodiscard]] auto fields_of(llvm::StructType* st) const -> std::vector<leaf> {
        const auto*       st_layout{layout_.getStructLayout(st)};
        std::vector<leaf> fields;
        const auto        add{[&](u32 i) {
            fields.emplace_back(st_layout->getElementOffset(i), st->getElementType(i));
        }};
        if (const auto it{padded_.find(st)}; it != padded_.end()) {
            for (const auto i : it->second.field_indices) { add(i); }
        } else {
            for (u32 i{0}; i < st->getNumElements(); ++i) { add(i); }
        }
        return fields;
    }

    // The members of an untagged union or the fields of a struct, each at its offset
    [[nodiscard]] auto record_fields(llvm::Type* ty) const -> stdx::option<std::vector<leaf>> {
        if (const auto* members{union_members_of(ty)}) {
            std::vector<leaf> fields;
            for (auto* member : *members) { fields.emplace_back(0, member); }
            return fields;
        }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) { return fields_of(st); }
        return stdx::none;
    }

    [[nodiscard]] static auto coerced(llvm::Type* original, std::vector<llvm::Type*> parts)
        -> value_plan {
        return {.kind     = pass_kind::COERCE,
                .original = original,
                .parts    = std::move(parts),
                .offsets  = {}};
    }

    [[nodiscard]] static auto direct(llvm::Type* original) -> value_plan {
        return {.kind = pass_kind::DIRECT, .original = original, .parts = {}, .offsets = {}};
    }

    [[nodiscard]] static auto memory(llvm::Type* original, pass_kind kind) -> value_plan {
        return {.kind = kind, .original = original, .parts = {}, .offsets = {}};
    }

    [[nodiscard]] auto plan_return(abi_model model, llvm::Type* ty) const -> value_plan {
        if (!is_by_value_aggregate(ty) || alloc_size(ty) == 0) { return direct(ty); }
        switch (model) {
        case abi_model::WIN64:          return win64(ty);
        case abi_model::SYSV_X86_64:    return sysv_return(ty);
        case abi_model::AAPCS64:
        case abi_model::DARWIN_AARCH64: return aarch64_return(ty);
        case abi_model::X86_32:         return x86_32_return(ty);
        case abi_model::AAPCS32:        return arm_return(ty, false);
        case abi_model::AAPCS32_VFP:    return arm_return(ty, true);
        case abi_model::RISCV:          {
            register_budget budget{.integer = 2, .sse = flen_ > 0 ? 2U : 0U};
            return riscv_aggregate(ty, budget, true, true);
        }
        case abi_model::NONE: break;
        }
        return direct(ty);
    }

    [[nodiscard]] auto
    plan_param(abi_model model, llvm::Type* ty, register_budget& budget, bool fixed) const
        -> value_plan {
        if (!is_by_value_aggregate(ty) || alloc_size(ty) == 0) {
            if (model == abi_model::SYSV_X86_64) { spend_scalar(ty, budget); }
            if (model == abi_model::RISCV) { riscv_spend_scalar(ty, budget, fixed); }
            return direct(ty);
        }
        switch (model) {
        case abi_model::WIN64:          return win64(ty);
        case abi_model::SYSV_X86_64:    return sysv_param(ty, budget);
        case abi_model::AAPCS64:        return aarch64_param(ty, false);
        case abi_model::DARWIN_AARCH64: return aarch64_param(ty, true);
        case abi_model::X86_32:         return x86_32_param(ty, fixed);
        case abi_model::AAPCS32:        return arm_param(ty, false);
        case abi_model::AAPCS32_VFP:    return arm_param(ty, true);
        case abi_model::RISCV:          return riscv_aggregate(ty, budget, fixed, false);
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
        if (!classes) { return sysv_byval(ty); }
        const auto count{[&](sysv_class wanted) {
            return static_cast<u32>(std::ranges::count(*classes, wanted));
        }};
        if (count(sysv_class::X87) + count(sysv_class::X87UP) > 0) { return sysv_byval(ty); }
        const auto int_regs{count(sysv_class::INTEGER)};
        const auto sse_regs{count(sysv_class::SSE)};
        if (int_regs > budget.integer || sse_regs > budget.sse) { return sysv_byval(ty); }
        auto parts{sysv_parts(ty, *classes)};
        if (parts.empty()) { return direct(ty); }
        budget.integer -= int_regs;
        budget.sse -= sse_regs;
        return coerced(ty, std::move(parts));
    }

    // A stack copy aligned to at least an eightbyte
    [[nodiscard]] auto sysv_byval(llvm::Type* ty) const -> value_plan {
        auto plan{memory(ty, pass_kind::BYVAL)};
        plan.byval_align = std::max<u64>(abi_align(ty), 8);
        return plan;
    }

    auto collect_leaves(llvm::Type* ty, u64 base, std::vector<leaf>& out) const -> void {
        if (const auto* members{union_members_of(ty)}) {
            for (auto* member : *members) { collect_leaves(member, base, out); }
        } else if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            for (const auto& field : fields_of(st)) {
                collect_leaves(field.type, base + field.offset, out);
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

    // 8 or 16 byte vectors and floating-point scalars can make up an HFA, though AAPCS32 only
    // takes `float` and `double`
    [[nodiscard]] auto is_hfa_base(llvm::Type* ty, bool arm32) const -> bool {
        if (ty->isVectorTy()) { return alloc_size(ty) == 8 || alloc_size(ty) == 16; }
        if (arm32) { return ty->isFloatTy() || ty->isDoubleTy(); }
        return ty->isFloatingPointTy() && !ty->isX86_FP80Ty() && !ty->isPPC_FP128Ty();
    }

    // clang's `isHomogeneousAggregate` count: struct members add up, union members take the
    // largest, and every scalar must match `base` in size and vector-ness
    [[nodiscard]] auto hfa_members(llvm::Type* ty, llvm::Type*& base, bool arm32) const
        -> stdx::option<u64> {
        if (const auto* members{union_members_of(ty)}) {
            u64 most{0};
            for (auto* member : *members) {
                const auto count{hfa_members(member, base, arm32)};
                if (!count) { return stdx::none; }
                most = std::max(most, *count);
            }
            return most;
        }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            u64 total{0};
            for (auto* element : st->elements()) {
                const auto count{hfa_members(element, base, arm32)};
                if (!count) { return stdx::none; }
                total += *count;
            }
            return total;
        }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            if (arr->getNumElements() == 0) { return stdx::none; }
            const auto count{hfa_members(arr->getElementType(), base, arm32)};
            if (!count) { return stdx::none; }
            return *count * arr->getNumElements();
        }
        if (!is_hfa_base(ty, arm32)) { return stdx::none; }
        if (!base) {
            base = ty;
        } else if (base->isVectorTy() != ty->isVectorTy() || alloc_size(base) != alloc_size(ty)) {
            return stdx::none;
        }
        return 1;
    }

    // A homogeneous floating-point aggregate: one to four members of a single base
    [[nodiscard]] auto homogeneous_member(llvm::Type* ty, bool arm32) const
        -> stdx::option<hfa_shape> {
        llvm::Type* base{nullptr};
        const auto  count{hfa_members(ty, base, arm32)};
        if (!count || !base || *count == 0 || *count > 4) { return stdx::none; }
        if (alloc_size(base) * *count != alloc_size(ty)) { return stdx::none; }
        return hfa_shape{.base = base, .members = *count};
    }

    [[nodiscard]] auto aarch64_pair(llvm::Type* ty) const -> llvm::Type* {
        if (abi_align(ty) >= 16) { return llvm::IntegerType::get(context_, 128); }
        return llvm::ArrayType::get(llvm::Type::getInt64Ty(context_), 2);
    }

    [[nodiscard]] auto aarch64_param(llvm::Type* ty, bool darwin) const -> value_plan {
        if (const auto hfa{homogeneous_member(ty, false)}) {
            auto plan{coerced(ty, {llvm::ArrayType::get(hfa->base, hfa->members)})};
            // Linux keeps an HFA's stack slot at 8 bytes, or 16 for a 16-byte aligned one
            if (!darwin) { plan.stack_align = abi_align(ty) >= 16 ? 16 : 8; }
            return plan;
        }
        const auto size{alloc_size(ty)};
        if (size <= 8) { return coerced(ty, {llvm::Type::getInt64Ty(context_)}); }
        if (size <= 16) { return coerced(ty, {aarch64_pair(ty)}); }
        return memory(ty, pass_kind::INDIRECT);
    }

    [[nodiscard]] auto aarch64_return(llvm::Type* ty) const -> value_plan {
        if (homogeneous_member(ty, false)) { return direct(ty); }
        const auto size{alloc_size(ty)};
        if (size <= 8) { return coerced(ty, {int_of_bytes(size)}); }
        if (size <= 16) { return coerced(ty, {aarch64_pair(ty)}); }
        return memory(ty, pass_kind::INDIRECT);
    }

    // i386 cdecl: a padding-free struct of 4 and 8 byte scalars up to 16 bytes travels as its
    // fields, anything else as a 4-byte aligned stack copy
    [[nodiscard]] auto x86_32_param(llvm::Type* ty, bool fixed) const -> value_plan {
        // MSVC passes an over-aligned aggregate to a fixed parameter by reference
        if (win32_struct_abi_ && fixed && abi_align(ty) > 8) {
            return memory(ty, pass_kind::INDIRECT);
        }
        if (const auto fields{x86_32_expansion(ty)}) {
            auto plan{coerced(ty, {})};
            for (const auto& [offset, field_ty] : *fields) {
                plan.parts.emplace_back(field_ty);
                plan.offsets.emplace_back(offset);
            }
            return plan;
        }
        auto plan{memory(ty, pass_kind::BYVAL)};
        plan.byval_align = 4;
        return plan;
    }

    // clang's `canExpandIndirectArgument`: every field a 32 or 64-bit scalar, with no padding
    [[nodiscard]] auto x86_32_expansion(llvm::Type* ty) const -> stdx::option<std::vector<leaf>> {
        if (alloc_size(ty) > 16) { return stdx::none; }
        auto fields{record_fields(ty)};
        if (!fields) { return stdx::none; }
        u64 total{0};
        for (const auto& field : *fields) {
            const auto size{alloc_size(field.type)};
            const bool scalar{field.type->isIntegerTy() || field.type->isPointerTy() ||
                              field.type->isFloatingPointTy()};
            if (!scalar || (size != 4 && size != 8)) { return stdx::none; }
            total += size;
        }
        if (total != alloc_size(ty)) { return stdx::none; }
        return fields;
    }

    [[nodiscard]] auto x86_32_return(llvm::Type* ty) const -> value_plan {
        if (!small_struct_in_regs_ || !x86_32_register_sized(ty)) {
            return memory(ty, pass_kind::INDIRECT);
        }
        // A lone float or pointer comes back as itself, except that MSVC keeps floats in EAX
        if (auto* element{single_element(ty)};
            element &&
            ((!win32_struct_abi_ && element->isFloatingPointTy()) || element->isPointerTy())) {
            return coerced(ty, {element});
        }
        return coerced(ty, {int_of_bytes(alloc_size(ty))});
    }

    // clang's `shouldReturnTypeInRegister`: 1, 2, 4, or 8 bytes, and so is every field inside
    [[nodiscard]] auto x86_32_register_sized(llvm::Type* ty) const -> bool {
        const auto size{alloc_size(ty)};
        if (size != 1 && size != 2 && size != 4 && size != 8) { return false; }
        if (ty->isVectorTy()) { return size != 8; }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            return x86_32_register_sized(arr->getElementType());
        }
        const auto fields{record_fields(ty)};
        if (!fields) { return true; }
        return std::ranges::all_of(*fields, [&](const leaf& field) {
            return alloc_size(field.type) == 0 || x86_32_register_sized(field.type);
        });
    }

    // clang's `isSingleElementStruct`: the one scalar a record wraps, through single-element
    // arrays and nested records, when nothing pads it
    [[nodiscard]] auto single_element(llvm::Type* ty) const -> llvm::Type* {
        const auto fields{record_fields(ty)};
        if (!fields) { return nullptr; }
        llvm::Type* found{nullptr};
        for (const auto& field : *fields) {
            if (alloc_size(field.type) == 0) { continue; }
            if (found) { return nullptr; }
            auto* inner{field.type};
            while (auto* arr{llvm::dyn_cast<llvm::ArrayType>(inner)}) {
                if (arr->getNumElements() != 1) { break; }
                inner = arr->getElementType();
            }
            found = is_by_value_aggregate(inner) ? single_element(inner) : inner;
            if (!found) { return nullptr; }
        }
        return found && alloc_size(found) == alloc_size(ty) ? found : nullptr;
    }

    // AAPCS: up to 64 bytes in core registers as words (or doublewords when 8-byte aligned),
    // larger on the stack, and with VFP an HFA in floating-point registers as itself
    [[nodiscard]] auto arm_param(llvm::Type* ty, bool vfp) const -> value_plan {
        if (vfp) {
            if (const auto hfa{homogeneous_member(ty, true)}) { return arm_hfa_param(ty, *hfa); }
        }
        const auto size{alloc_size(ty)};
        const auto align{abi_align(ty)};
        if (size > 64) {
            auto plan{memory(ty, pass_kind::BYVAL)};
            plan.byval_align = std::clamp<u64>(align, 4, 8);
            return plan;
        }
        const u64 word{align <= 4 ? 4U : 8U};
        return coerced(ty, {llvm::ArrayType::get(int_of_bytes(word), (size + word - 1) / word)});
    }

    // An over-aligned HFA keeps an 8-byte aligned stack slot
    [[nodiscard]] auto arm_hfa_param(llvm::Type* ty, const hfa_shape& hfa) const -> value_plan {
        if (abi_align(ty) <= abi_align(hfa.base) || abi_align(ty) < 8) { return direct(ty); }
        auto plan{coerced(ty, {ty})};
        plan.stack_align = 8;
        return plan;
    }

    // AAPCS returns at most 4 bytes in r0, as the narrowest integer on little-endian targets
    [[nodiscard]] auto arm_return(llvm::Type* ty, bool vfp) const -> value_plan {
        if (vfp && homogeneous_member(ty, true)) { return direct(ty); }
        const auto size{alloc_size(ty)};
        if (size > 4) { return memory(ty, pass_kind::INDIRECT); }
        if (layout_.isBigEndian() || size > 2) {
            return coerced(ty, {llvm::Type::getInt32Ty(context_)});
        }
        return coerced(ty, {int_of_bytes(size)});
    }

    // One field of a struct that RISC-V passes in floating-point registers
    struct fp_field {
        u64         offset;
        llvm::Type* type;
    };

    // The registers a RISC-V argument of `bits` takes from the integer budget
    auto riscv_spend_gprs(u64 bits, u64 align_bits, bool fixed, register_budget& budget) const
        -> void {
        u32 needed{1};
        if (!fixed && align_bits == 2 * xlen_) {
            needed = 2 + (riscv_e_ && xlen_ == 32 ? 0 : budget.integer % 2);
        } else if (bits > xlen_ && bits <= 2 * xlen_) {
            needed = 2;
        }
        budget.integer -= std::min(needed, budget.integer);
    }

    auto riscv_spend_scalar(llvm::Type* ty, register_budget& budget, bool fixed) const -> void {
        if (ty->isVoidTy()) { return; }
        const auto bits{alloc_size(ty) * 8};
        if (fixed && ty->isFloatingPointTy() && bits <= flen_ && budget.sse > 0) {
            --budget.sse;
            return;
        }
        riscv_spend_gprs(bits, abi_align(ty) * 8, fixed, budget);
    }

    // clang's `detectFPCCEligibleStructHelper`: gathers at most two scalars, at most one of
    // them an integer, each no wider than its register
    [[nodiscard]] auto riscv_gather(llvm::Type* ty, u64 offset, std::vector<fp_field>& fields) const
        -> bool {
        if (ty->isIntegerTy() || ty->isFloatingPointTy()) {
            const auto bits{alloc_size(ty) * 8};
            if (ty->isIntegerTy() && bits > xlen_) { return false; }
            if (ty->isFloatingPointTy() && bits > flen_) { return false; }
            if (ty->isIntegerTy() && !fields.empty() && fields.front().type->isIntegerTy()) {
                return false;
            }
            if (fields.size() == 2) { return false; }
            fields.emplace_back(offset, ty);
            return true;
        }
        if (auto* arr{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            const auto stride{alloc_size(arr->getElementType())};
            for (u64 i{0}; i < arr->getNumElements(); ++i) {
                if (!riscv_gather(arr->getElementType(), offset + (i * stride), fields)) {
                    return false;
                }
            }
            return true;
        }
        if (!ty->isStructTy()) { return false; }
        // An empty record contributes nothing, and a union disqualifies the whole struct
        if (alloc_size(ty) == 0) { return true; }
        if (union_members_of(ty)) { return false; }
        for (const auto& field : fields_of(llvm::cast<llvm::StructType>(ty))) {
            if (!riscv_gather(field.type, offset + field.offset, fields)) { return false; }
        }
        return !fields.empty();
    }

    // A struct that flattens to one float, two floats, or an integer and a float
    [[nodiscard]] auto riscv_fp_fields(llvm::Type* ty) const
        -> stdx::option<std::vector<fp_field>> {
        if (union_members_of(ty)) { return stdx::none; }
        std::vector<fp_field> fields;
        if (!riscv_gather(ty, 0, fields) || fields.empty()) { return stdx::none; }
        if (fields.size() == 1 && !fields.front().type->isFloatingPointTy()) { return stdx::none; }
        return fields;
    }

    // RISC-V: fixed arguments flatten into floating-point registers while they last, then
    // anything up to two registers wide travels as integers and the rest by reference
    [[nodiscard]] auto
    riscv_aggregate(llvm::Type* ty, register_budget& budget, bool fixed, bool is_return) const
        -> value_plan {
        if (fixed && flen_ > 0) {
            if (const auto fields{riscv_fp_fields(ty)}) {
                const auto floats{
                    static_cast<u32>(std::ranges::count_if(*fields, [](const fp_field& field) {
                        return field.type->isFloatingPointTy();
                    }))};
                const auto ints{static_cast<u32>(fields->size()) - floats};
                if (ints <= budget.integer && floats <= budget.sse) {
                    budget.integer -= ints;
                    budget.sse -= floats;
                    return riscv_flattened(ty, *fields, is_return);
                }
            }
        }
        const auto bits{alloc_size(ty) * 8};
        riscv_spend_gprs(bits, abi_align(ty) * 8, fixed, budget);
        if (bits > 2 * xlen_) { return memory(ty, pass_kind::INDIRECT); }
        if (bits <= xlen_) { return coerced(ty, {int_of_bytes(bits / 8)}); }
        auto* word{llvm::IntegerType::get(context_, xlen_)};
        if (abi_align(ty) * 8 == 2 * xlen_) {
            return coerced(ty, {llvm::IntegerType::get(context_, 2 * xlen_)});
        }
        return coerced(ty, {llvm::ArrayType::get(word, 2)});
    }

    // clang's `coerceAndExpandFPCCEligibleStruct`: each field its own argument, and a result
    // of two comes back as a struct of both, packed when the second sits misaligned
    [[nodiscard]] auto riscv_flattened(llvm::Type*                  ty,
                                       const std::vector<fp_field>& fields,
                                       bool                         is_return) const -> value_plan {
        auto plan{coerced(ty, {})};
        for (const auto& field : fields) {
            plan.parts.emplace_back(field.type);
            plan.offsets.emplace_back(field.offset);
        }
        if (is_return && fields.size() == 2) {
            const auto& second{fields.back()};
            const bool  packed{second.offset % abi_align(second.type) != 0};
            plan.parts = {llvm::StructType::get(context_, plan.parts, packed)};
        }
        return plan;
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
    const padded_struct_map& padded_;
    bool                     small_struct_in_regs_{false};
    bool                     win32_struct_abi_{false};
    u32                      xlen_{0};
    u32                      flen_{0};
    bool                     riscv_e_{false};
    u32                      riscv_gprs_{8};
};

// Moves values between their ghoti and C ABI shapes through stack memory, as clang does, and
// leaves the round trips for later passes to fold
class abi_rewriter {
  public:
    abi_rewriter(llvm::Module&            module,
                 const union_member_map&  unions,
                 const padded_struct_map& padded)
        : module_{module}, layout_{module.getDataLayout()}, context_{module.getContext()},
          classifier_{module, unions, padded}, copier_{module} {}

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

    // The types at a plan's offsets: its parts, or the elements of its one scattered struct
    [[nodiscard]] static auto pieces_of(const value_plan& plan) -> std::vector<llvm::Type*> {
        if (plan.offsets.size() == plan.parts.size()) { return plan.parts; }
        const auto elements{llvm::cast<llvm::StructType>(plan.parts.front())->elements()};
        return {elements.begin(), elements.end()};
    }

    [[nodiscard]] auto piece_address(llvm::IRBuilder<>& builder, llvm::AllocaInst* slot, u64 offset)
        -> std::pair<llvm::Value*, llvm::Align> {
        auto* at{builder.CreateConstInBoundsGEP1_64(builder.getInt8Ty(), slot, offset)};
        return {at, llvm::commonAlignment(slot->getAlign(), offset)};
    }

    [[nodiscard]] auto split(llvm::IRBuilder<>& builder, llvm::Value* value, const value_plan& plan)
        -> std::vector<llvm::Value*> {
        auto [slot, parts_ty]{coercion_slot(*builder.GetInsertBlock()->getParent(), plan)};
        builder.CreateStore(value, slot);
        if (!plan.offsets.empty()) { return split_scattered(builder, slot, plan); }
        std::vector<llvm::Value*> parts;
        for (u32 i{0}; i < plan.parts.size(); ++i) {
            auto* at{builder.CreateStructGEP(parts_ty, slot, i)};
            parts.emplace_back(builder.CreateLoad(plan.parts[i], at, "abi.part"));
        }
        return parts;
    }

    [[nodiscard]] auto split_scattered(llvm::IRBuilder<>& builder,
                                       llvm::AllocaInst*  slot,
                                       const value_plan&  plan) -> std::vector<llvm::Value*> {
        const auto                pieces{pieces_of(plan)};
        std::vector<llvm::Value*> loaded;
        for (usize i{0}; i < pieces.size(); ++i) {
            const auto [at, align]{piece_address(builder, slot, plan.offsets[i])};
            loaded.emplace_back(builder.CreateAlignedLoad(pieces[i], at, align, "abi.part"));
        }
        if (pieces.size() == plan.parts.size()) { return loaded; }
        llvm::Value* whole{llvm::PoisonValue::get(plan.parts.front())};
        for (u32 i{0}; i < loaded.size(); ++i) {
            whole = builder.CreateInsertValue(whole, loaded[i], i);
        }
        return {whole};
    }

    [[nodiscard]] auto join(llvm::IRBuilder<>&      builder,
                            std::span<llvm::Value*> parts,
                            const value_plan&       plan) -> llvm::Value* {
        auto [slot, parts_ty]{coercion_slot(*builder.GetInsertBlock()->getParent(), plan)};
        if (!plan.offsets.empty()) {
            join_scattered(builder, slot, parts, plan);
            return builder.CreateLoad(plan.original, slot, "abi.joined");
        }
        for (usize i{0}; i < parts.size(); ++i) {
            builder.CreateStore(parts[i],
                                builder.CreateStructGEP(parts_ty, slot, static_cast<u32>(i)));
        }
        return builder.CreateLoad(plan.original, slot, "abi.joined");
    }

    auto join_scattered(llvm::IRBuilder<>&      builder,
                        llvm::AllocaInst*       slot,
                        std::span<llvm::Value*> parts,
                        const value_plan&       plan) -> void {
        const auto                pieces{pieces_of(plan)};
        std::vector<llvm::Value*> values{parts.begin(), parts.end()};
        if (pieces.size() != plan.parts.size()) {
            values.clear();
            for (u32 i{0}; i < pieces.size(); ++i) {
                values.emplace_back(builder.CreateExtractValue(parts.front(), i));
            }
        }
        for (usize i{0}; i < values.size(); ++i) {
            const auto [at, align]{piece_address(builder, slot, plan.offsets[i])};
            builder.CreateAlignedStore(values[i], at, align);
        }
    }

    [[nodiscard]] auto memory_align(llvm::Type* ty) const -> llvm::Align {
        return std::max(layout_.getABITypeAlign(ty), llvm::Align{8});
    }

    [[nodiscard]] auto memory_param_attrs(const value_plan& plan) -> llvm::AttributeSet {
        llvm::AttrBuilder attrs{context_};
        if (plan.kind == pass_kind::BYVAL) {
            attrs.addByValAttr(plan.original);
            attrs.addAlignmentAttr(plan.byval_align > 0 ? llvm::Align{plan.byval_align}
                                                        : memory_align(plan.original));
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
                if (param.byval_align > 0) {
                    clamp_reads(*pointer, llvm::Align{param.byval_align});
                }
                break;
            }
            }
        }
    }

    // A `byval` copy the ABI aligns below its type is read no more aligned than it arrives
    static auto clamp_reads(llvm::Argument& pointer, llvm::Align align) -> void {
        for (auto* user : pointer.users()) {
            if (auto* load{llvm::dyn_cast<llvm::LoadInst>(user)}) {
                load->setAlignment(std::min(load->getAlign(), align));
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

auto to_llvm_callconv(ast::calling_convention conv, const llvm::Triple& triple) noexcept
    -> llvm::CallingConv::ID {
    switch (conv) {
    case ast::calling_convention::X86_WINAPI:
        return triple.getArch() == llvm::Triple::x86 ? llvm::CallingConv::X86_StdCall
                                                     : llvm::CallingConv::C;
    case ast::calling_convention::C:            return llvm::CallingConv::C;
    case ast::calling_convention::SYSV:         return llvm::CallingConv::X86_64_SysV;
    case ast::calling_convention::WIN64:        return llvm::CallingConv::Win64;
    case ast::calling_convention::X86_STDCALL:  return llvm::CallingConv::X86_StdCall;
    case ast::calling_convention::X86_FASTCALL: return llvm::CallingConv::X86_FastCall;
    case ast::calling_convention::AAPCS:        return llvm::CallingConv::ARM_AAPCS;
    default:                                    return llvm::CallingConv::C;
    }
}

auto has_c_abi_model(const llvm::Triple& triple, llvm::CallingConv::ID conv) -> bool {
    return model_for(triple, conv) != abi_model::NONE;
}

auto lower_c_abi(llvm::Module&            module,
                 const union_member_map&  unions,
                 const padded_struct_map& padded) -> void {
    PROFILE_FUNCTION();
    abi_rewriter{module, unions, padded}.run();
}

} // namespace ghoti::codegen
