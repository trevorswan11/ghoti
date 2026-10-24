#include "compiler/codegen/llvm_lowering.hh"
#include "compiler/codegen/runtime_libcalls.hh"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CallingConv.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/AtomicOrdering.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/codegen/aggregate_abi.hh"
#include "compiler/codegen/c_abi.hh"
#include "compiler/codegen/type_translator.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "support/diagnostic.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

namespace ghoti::codegen {

namespace {

// Marks a `@[testing]` helper so calls to it check whether it aborted its test
constexpr std::string_view TESTING_FN_ATTRIBUTE{"ghoti-testing"};

// Local symbols are invisible to the linker, and LLVM requires them to keep default visibility
auto apply_visibility(llvm::GlobalValue& value, stdx::option<ast::symbol_visibility> visibility)
    -> void {
    if (!visibility || value.hasLocalLinkage()) { return; }
    switch (*visibility) {
    case ast::symbol_visibility::DEFAULT:
        value.setVisibility(llvm::GlobalValue::DefaultVisibility);
        break;
    case ast::symbol_visibility::HIDDEN:
        value.setVisibility(llvm::GlobalValue::HiddenVisibility);
        break;
    case ast::symbol_visibility::PROTECTED:
        value.setVisibility(llvm::GlobalValue::ProtectedVisibility);
        break;
    }
}

// The little-endian bytes of a scalar union payload, zero past its width
[[nodiscard]] auto union_payload_bytes(const gir::const_value& payload, usize size) -> std::string {
    std::string bytes(size, '\0');
    if (const auto iv{payload.as_int_opt()}) {
        const u64   v{static_cast<u64>(*iv)};
        const usize copy_len{std::min<usize>(sizeof(v), size)};
        std::memcpy(bytes.data(), &v, copy_len);
    } else if (const auto fv{payload.as_opt<f128>()}) {
        const auto payload_type{payload.get_type()};
        const auto format{payload_type ? sema::float_format_of(*payload_type) : stdx::none};
        const auto encoded{fv->encode(format.value_or(float_format::DOUBLE))};
        const auto width{format_info(format.value_or(float_format::DOUBLE)).storage_bits};
        const std::array<u64, 2> words{encoded.low, encoded.high};
        const usize              copy_len{std::min<usize>(width / 8, size)};
        std::memcpy(bytes.data(), words.data(), copy_len);
    } else if (const auto bv{payload.as_opt<bool>()}) {
        if (size > 0) { bytes[0] = *bv ? 1 : 0; }
    }
    return bytes;
}

// Reads `source`'s bytes back as a constant of `ty` starting `offset` bytes in, one scalar at a
// time, with zeroes past its end
[[nodiscard]] auto
reinterpret_constant(llvm::Constant* source, llvm::Type* ty, u64 offset, const llvm::DataLayout& dl)
    -> llvm::Constant* {
    if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
        const auto*                  layout{dl.getStructLayout(st)};
        std::vector<llvm::Constant*> elems;
        for (u32 i{0}; i < st->getNumElements(); ++i) {
            elems.emplace_back(reinterpret_constant(
                source, st->getElementType(i), offset + layout->getElementOffset(i), dl));
        }
        return llvm::ConstantStruct::get(st, elems);
    }
    if (auto* at{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
        const auto stride{dl.getTypeAllocSize(at->getElementType()).getFixedValue()};
        std::vector<llvm::Constant*> elems;
        for (u64 i{0}; i < at->getNumElements(); ++i) {
            elems.emplace_back(
                reinterpret_constant(source, at->getElementType(), offset + (i * stride), dl));
        }
        return llvm::ConstantArray::get(at, elems);
    }
    if (offset >= dl.getTypeAllocSize(source->getType()).getFixedValue()) {
        return llvm::Constant::getNullValue(ty);
    }
    auto* folded{llvm::ConstantFoldLoadFromConst(source, ty, llvm::APInt{64, offset}, dl)};
    if (!folded || llvm::isa<llvm::PoisonValue, llvm::UndefValue>(folded)) {
        return llvm::Constant::getNullValue(ty);
    }
    return folded;
}

// Which narrow integers a C caller extends to 32 bits for its callee, as clang does per target.
// RV64 also sign-extends every 32-bit integer, unsigned ones included, to the full register
enum class narrow_int_rule : u8 {
    NONE,
    BOOL_ONLY,
    ALL,
    ALL_AND_RV64_WORD,
};

[[nodiscard]] auto narrow_int_rule_for(const llvm::Triple& triple) -> narrow_int_rule {
    if (triple.getArch() == llvm::Triple::x86_64) {
        return triple.isOSWindows() || triple.isUEFI() ? narrow_int_rule::BOOL_ONLY
                                                       : narrow_int_rule::ALL;
    }
    if (triple.isAArch64()) {
        return triple.isOSDarwin() ? narrow_int_rule::ALL : narrow_int_rule::NONE;
    }
    if (triple.isRISCV64()) { return narrow_int_rule::ALL_AND_RV64_WORD; }
    if (triple.getArch() == llvm::Triple::x86 || triple.isARM() || triple.isThumb() ||
        triple.isRISCV32()) {
        return narrow_int_rule::ALL;
    }
    return narrow_int_rule::NONE;
}

auto apply_fn_attributes(llvm::Function& llvm_fn, const sema::resolved_attributes& attributes)
    -> void {
    apply_visibility(llvm_fn, attributes.visibility);
    if (attributes.naked) { llvm_fn.addFnAttr(llvm::Attribute::Naked); }
    if (attributes.cold) { llvm_fn.addFnAttr(llvm::Attribute::Cold); }
    // Keeps loop-idiom recognition from turning a hand-written `memcpy` loop into a `memcpy` call
    if (attributes.no_builtins) { llvm_fn.addFnAttr("no-builtins"); }
    // Joined with the target's own features once the target machine is known
    if (!attributes.target_features.empty()) {
        llvm_fn.addFnAttr("ghoti-target-features",
                          fmt::format("{}", fmt::join(attributes.target_features, ",")));
    }
    if (attributes.alignment) { llvm_fn.setAlignment(llvm::Align{*attributes.alignment}); }
    if (!attributes.inlining) { return; }
    switch (*attributes.inlining) {
    case ast::inline_mode::ALWAYS:  llvm_fn.addFnAttr(llvm::Attribute::AlwaysInline); break;
    case ast::inline_mode::NEVER:   llvm_fn.addFnAttr(llvm::Attribute::NoInline); break;
    case ast::inline_mode::HINT:    llvm_fn.addFnAttr(llvm::Attribute::InlineHint); break;
    case ast::inline_mode::DEFAULT: break;
    }
}

// A `comptime_float` materializes as `f64`; a `comptime_int` as `i32`.
[[nodiscard]] auto materialized_is_float(sema::type_kind k) noexcept -> bool {
    return sema::is_float(k) || k == sema::type_kind::COMPTIME_FLOAT;
}

// Neither `comptime_int` nor `comptime_float` has a concrete width/kind of its own
[[nodiscard]] auto is_untyped_comptime(sema::type_kind k) noexcept -> bool {
    return k == sema::type_kind::COMPTIME_INT || k == sema::type_kind::COMPTIME_FLOAT;
}

[[nodiscard]] auto is_comptime_only_signature(const sema::types::function& fn_data) noexcept
    -> bool {
    return fn_data.return_type.get_kind() == sema::type_kind::TYPE;
}

// A function over compile-time-only values (`Option(type)`'s methods, say) has no runtime form
[[nodiscard]] auto is_comptime_only(const gir::function& fn) -> bool {
    const auto fn_data{sema::signature_of(fn.get_type())};
    return fn.get_linkage() != gir::linkage::EXTERN && fn_data &&
           is_comptime_only_signature(*fn_data);
}

// The concrete peer type two operands should lower against, or none if both are
// `is_untyped_comptime`
[[nodiscard]] auto concrete_peer_type(stdx::option<sema::type&> op0_ty,
                                      stdx::option<sema::type&> op1_ty)
    -> stdx::option<sema::type&> {
    if (op0_ty && !is_untyped_comptime(op0_ty->get_kind())) { return op0_ty; }
    if (op1_ty && !is_untyped_comptime(op1_ty->get_kind())) { return op1_ty; }
    return stdx::none;
}

[[nodiscard]] auto is_float_type(const gir::instruction&    inst,
                                 stdx::option<llvm::Value&> val) noexcept -> bool {
    // Any float (or `comptime_float`) operand or lane makes the operation floating point.
    for (const auto& op : inst.operands) {
        if (op.type && materialized_is_float(sema::lane_type_of(*op.type).get_kind())) {
            return true;
        }
    }
    if (!inst.operands.empty() && inst.operands[0].type &&
        sema::is_integer(sema::lane_type_of(*inst.operands[0].type).get_kind())) {
        return false;
    }
    if (inst.type) { return materialized_is_float(sema::lane_type_of(*inst.type).get_kind()); }
    return val && val->getType()->isFPOrFPVectorTy();
}

[[nodiscard]] auto comptime_operand_is_nonnegative(const gir::value& val) noexcept -> bool {
    if (const auto v{val.as_opt<i64>()}) { return *v >= 0; }
    if (const auto v{val.as_opt<i128>()}) { return *v >= 0; }
    return val.is<u64>() || val.is<u128>();
}

[[nodiscard]] auto is_signed_type(const gir::instruction& inst) noexcept -> bool {
    if (!inst.operands.empty() && inst.operands[0].type &&
        inst.operands[0].type->get_kind() == sema::type_kind::COMPTIME_INT) {
        // `comptime_int` alone carries no real signedness
        for (const auto& op : inst.operands | std::views::drop(1)) {
            if (op.type && op.type->get_kind() != sema::type_kind::COMPTIME_INT) {
                return sema::is_signed_integer(*op.type);
            }
        }
        return !std::ranges::all_of(
            inst.operands, [](const auto& op) { return comptime_operand_is_nonnegative(op); });
    }
    if (!inst.operands.empty() && inst.operands[0].type) {
        return sema::is_signed_integer(sema::lane_type_of(*inst.operands[0].type));
    }
    if (inst.type) { return sema::is_signed_integer(sema::lane_type_of(*inst.type)); }
    return false;
}

// Build an integer constant of `ty`'s width from a full 128-bit value.
[[nodiscard]] auto wide_int_constant(u128 v, llvm::Type* ty) -> llvm::Constant* {
    const std::array<u64, 2> words{static_cast<u64>(v), static_cast<u64>(v >> 64)};
    const llvm::APInt        value{128, words};
    return llvm::ConstantInt::get(ty->getContext(), value.zextOrTrunc(ty->getIntegerBitWidth()));
}

// An integer-valued constant whose GIR type is a pointer must materialize as an `inttoptr` constant
[[nodiscard]] auto int_or_ptr_constant(u128 bits, llvm::Type* ty) -> llvm::Constant* {
    // `u0`'s only value
    if (ty->isStructTy()) { return llvm::ConstantAggregateZero::get(ty); }
    if (!ty->isPointerTy()) { return wide_int_constant(bits, ty); }
    if (bits == 0) { return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ty)); }
    auto* int_ty{llvm::Type::getInt64Ty(ty->getContext())};
    return llvm::ConstantExpr::getIntToPtr(wide_int_constant(bits, int_ty), ty);
}

// The float constant of `ty` (or its elements) nearest to `value`
// `half` and vectors read as `f32`, which LLVM's own names already serve
[[nodiscard]] auto llvm_float_format(const llvm::Type* type) -> float_format {
    return type->isFP128Ty()      ? float_format::QUAD
           : type->isX86_FP80Ty() ? float_format::X87
           : type->isDoubleTy()   ? float_format::DOUBLE
                                  : float_format::SINGLE;
}

[[nodiscard]] auto float_constant(f128 value, llvm::Type* ty) -> llvm::Constant* {
    auto* scalar_ty{ty->getScalarType()};
    VERIFY(scalar_ty->isFloatingPointTy(), "A float payload must lower to a float type");
    // `APFloat::convert` quiets a signaling NaN, so a NaN is built from its exact encoding
    if (value.is_nan() && !scalar_ty->isPPC_FP128Ty() && !scalar_ty->isBFloatTy()) {
        const auto               format{scalar_ty->isHalfTy() ? float_format::HALF
                                                              : llvm_float_format(scalar_ty)};
        const auto               bits{value.encode(format)};
        const std::array<u64, 2> nan_words{bits.low, bits.high};
        const auto width{static_cast<u32>(scalar_ty->getPrimitiveSizeInBits().getFixedValue())};
        const llvm::APFloat nan{scalar_ty->getFltSemantics(), llvm::APInt{width, nan_words}};
        return llvm::ConstantFP::get(ty, nan);
    }
    const std::array<u64, 2> words{value.bits().low, value.bits().high};
    llvm::APFloat            converted{llvm::APFloat::IEEEquad(), llvm::APInt{128, words}};
    bool                     loses_info{false};
    DISCARD(converted.convert(
        scalar_ty->getFltSemantics(), llvm::APFloat::rmNearestTiesToEven, &loses_info));
    return llvm::ConstantFP::get(ty, converted);
}

enum class memory_order_t : u8 { // Mirrors builtin.gh.inc
    relaxed = 0,
    acquire = 1,
    release = 2,
    acq_rel = 3,
    seq_cst = 4,
};

// `ord` is a `MemoryOrder` enum ordinal as folded from the atomic builtin
[[nodiscard]] auto to_llvm_ordering(u8 ord) noexcept -> llvm::AtomicOrdering {
    switch (static_cast<memory_order_t>(ord)) {
    case memory_order_t::relaxed: return llvm::AtomicOrdering::Monotonic;
    case memory_order_t::acquire: return llvm::AtomicOrdering::Acquire;
    case memory_order_t::release: return llvm::AtomicOrdering::Release;
    case memory_order_t::acq_rel: return llvm::AtomicOrdering::AcquireRelease;
    case memory_order_t::seq_cst:
    default:                      return llvm::AtomicOrdering::SequentiallyConsistent;
    }
}

enum class atomic_rmw_op_t : u8 { // Mirrors builtin.gh.inc
    xchg = 0,
    add  = 1,
    sub  = 2,
    band = 3,
    nand = 4,
    bor  = 5,
    bxor = 6,
    max  = 7,
    min  = 8,
    umax = 9,
    umin = 10,
};

// An atomic access's size as its alignment, which is what `atomicrmw`/`cmpxchg` assume
[[nodiscard]] auto natural_atomic_align(const llvm::Module& module, llvm::Type* type)
    -> llvm::Align {
    return llvm::Align{llvm::PowerOf2Ceil(module.getDataLayout().getTypeStoreSize(type))};
}

// `op` is an `AtomicRmwOp` enum ordinal

[[nodiscard]] auto to_llvm_rmw_op(u8 op) noexcept -> llvm::AtomicRMWInst::BinOp {
    using Op = llvm::AtomicRMWInst::BinOp;
    switch (static_cast<atomic_rmw_op_t>(op)) {
    case atomic_rmw_op_t::xchg: return Op::Xchg;
    case atomic_rmw_op_t::add:  return Op::Add;
    case atomic_rmw_op_t::sub:  return Op::Sub;
    case atomic_rmw_op_t::band: return Op::And;
    case atomic_rmw_op_t::nand: return Op::Nand;
    case atomic_rmw_op_t::bor:  return Op::Or;
    case atomic_rmw_op_t::bxor: return Op::Xor;
    case atomic_rmw_op_t::max:  return Op::Max;
    case atomic_rmw_op_t::min:  return Op::Min;
    case atomic_rmw_op_t::umax: return Op::UMax;
    case atomic_rmw_op_t::umin:
    default:                    return Op::UMin;
    }
}

} // namespace

llvm_lowering::llvm_lowering(llvm::LLVMContext& context, std::string_view module_name) noexcept
    : context_{context}, llvm_module_{stdx::make_box<llvm::Module>(module_name, context_)},
      builder_{context_}, types_{context_, *llvm_module_} {}

auto llvm_lowering::to_ir_string(const llvm::Module& mod) -> std::string {
    PROFILE_FUNCTION();
    std::string              out;
    llvm::raw_string_ostream os{out};
    mod.print(os, nullptr);
    return out;
}

// Every function is declared up front so bodies and vtables can reference any of them
auto llvm_lowering::declare_functions(const gir::module_t& gir_mod) -> void {
    PROFILE_SCOPE("llvm_lowering: declare functions");
    for (const auto fn : gir_mod.get_functions()) {
        if (!is_comptime_only(*fn)) { declare_function(*fn); }
    }
}

auto llvm_lowering::lower_definitions(const gir::module_t& gir_mod) -> void {
    {
        PROFILE_SCOPE("llvm_lowering: lower vtables");
        lower_dyn_vtables(gir_mod);
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower globals");
        for (const auto global : gir_mod.get_globals()) { lower_global(*global); }
    }
    {
        PROFILE_SCOPE("llvm_lowering: lower functions");
        for (const auto fn : gir_mod.get_functions()) {
            if (!is_comptime_only(*fn)) { lower_function(*fn); }
        }
    }
}

auto llvm_lowering::lower(const gir::module_t& gir_mod) -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    gir_module_.emplace(gir_mod);
    declare_functions(gir_mod);
    lower_definitions(gir_mod);
    finalize_runtime_support();
    return std::move(llvm_module_);
}

auto llvm_lowering::finalize_runtime_support() -> void {
    lower_c_abi(*llvm_module_, types_.union_members(), types_.padded_structs());
    lower_large_aggregates(*llvm_module_);
    maybe_emit_mingw_main_stub();
    // ghoti never unwinds; without this, ARM EHABI references libgcc's `__aeabi_unwind_cpp_pr*`
    for (auto& fn : *llvm_module_) { fn.addFnAttr(llvm::Attribute::NoUnwind); }
}

// MinGW codegen injects a `__main` (static-ctor hook) call into any function named `main`
auto llvm_lowering::maybe_emit_mingw_main_stub() -> void {
    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (!triple.isWindowsGNUEnvironment() || llvm_module_->getFunction("__main")) { return; }
    const auto* main_fn{llvm_module_->getFunction("main")};
    if (!main_fn || main_fn->isDeclaration()) { return; }

    auto*             stub_ty{llvm::FunctionType::get(llvm::Type::getVoidTy(context_), false)};
    auto*             stub{llvm::Function::Create(
        stub_ty, llvm::Function::ExternalLinkage, "__main", llvm_module_.get())};
    llvm::IRBuilder<> stub_builder{llvm::BasicBlock::Create(context_, "entry", stub)};
    stub_builder.CreateRetVoid();
    llvm::appendToUsed(*llvm_module_, {stub});
}

auto llvm_lowering::lower_dyn_vtables(const gir::module_t& gir_mod) -> void {
    PROFILE_FUNCTION();
    auto* ptr_ty{types_.get_ptr_ty()};
    for (const auto& vt : gir_mod.get_ast_module().dyn_vtables) {
        if (llvm_module_->getNamedGlobal(vt.symbol)) { continue; }
        std::vector<llvm::Constant*> slots;
        slots.reserve(vt.slots.size());
        for (const auto& name : vt.slots) {
            auto* fn{llvm_module_->getFunction(name)};
            slots.emplace_back(fn ? llvm::cast<llvm::Constant>(fn)
                                  : llvm::ConstantPointerNull::get(ptr_ty));
        }

        auto* arr_ty{llvm::ArrayType::get(ptr_ty, slots.size())};
        new llvm::GlobalVariable(*llvm_module_,
                                 arr_ty,
                                 true,
                                 llvm::GlobalValue::InternalLinkage,
                                 llvm::ConstantArray::get(arr_ty, slots),
                                 vt.symbol);
    }
}

auto llvm_lowering::lower_executable(const gir::module_t& gir_mod, std::string_view user_main_name)
    -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    is_executable_  = true;
    user_main_name_ = user_main_name;
    gir_module_.emplace(gir_mod);
    declare_functions(gir_mod);
    lower_definitions(gir_mod);
    finalize_runtime_support();
    return std::move(llvm_module_);
}

auto llvm_lowering::windows_stack_probe_symbol() const -> stdx::option<std::string_view> {
    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (!triple.isOSWindows()) { return stdx::none; }
    if (triple.getArch() == llvm::Triple::x86_64) {
        return triple.isOSCygMing() ? "___chkstk_ms" : "__chkstk";
    }
    if (triple.getArch() == llvm::Triple::x86) {
        return triple.isOSCygMing() ? "_alloca" : "_chkstk";
    }
    return stdx::none;
}

auto llvm_lowering::usize_const(u64 value) -> llvm::ConstantInt* {
    return llvm::ConstantInt::get(types_.get_usize_ty(), value);
}

auto llvm_lowering::lower_test_executable(const gir::module_t& gir_mod,
                                          std::string_view     user_main_name)
    -> stdx::box<llvm::Module> {
    PROFILE_FUNCTION();
    is_executable_  = true;
    user_main_name_ = user_main_name;
    gir_module_.emplace(gir_mod);
    declare_functions(gir_mod);
    define_test_take_skipped();
    lower_definitions(gir_mod);
    finalize_runtime_support();
    return std::move(llvm_module_);
}

// `@testFunctions()`: a `[]builtin.Test` over every test function, built once
auto llvm_lowering::emit_test_table() -> llvm::Value* {
    PROFILE_FUNCTION();
    auto* slice_ty{types_.translate_slice_type()};
    if (auto* table{llvm_module_->getGlobalVariable("__ghoti_tests_slice", true)}) {
        return builder_.CreateLoad(slice_ty, table, "tests.slice");
    }

    // Mirrors `builtin.SourceLocation { file: []u8, line: u32, column: u32 }`
    auto* loc_struct_ty{
        llvm::StructType::get(context_, {slice_ty, types_.get_int32_ty(), types_.get_int32_ty()})};
    // Mirrors `builtin.Test { name: []u8, loc: SourceLocation, func: extern fn(): bool }`
    auto* test_struct_ty{
        llvm::StructType::get(context_, {slice_ty, loc_struct_ty, types_.get_ptr_ty()})};

    const auto& test_fns{gir_module_->get_test_functions()};
    const auto  test_count{test_fns.size()};

    const auto string_slice{[&](std::string_view text, std::string_view name) {
        auto* data{llvm::ConstantDataArray::getString(context_, text, false)};
        auto* gvar{new llvm::GlobalVariable(
            *llvm_module_, data->getType(), true, llvm::GlobalValue::InternalLinkage, data, name)};
        return llvm::ConstantStruct::get(
            slice_ty, {gvar, llvm::ConstantInt::get(types_.get_usize_ty(), text.size())});
    }};

    std::vector<llvm::Constant*> descriptors;
    descriptors.reserve(test_count);
    for (usize i{0}; i < test_count; ++i) {
        const auto fn{test_fns[i]};
        auto*      test_llvm_fn{llvm_module_->getFunction(fn->get_name())};
        ASSERT(test_llvm_fn, "Test function must already be declared before the test table");

        const auto& desc{fn->get_test_desc().empty() ? fn->get_name() : fn->get_test_desc()};
        const auto  file{fn->get_test_file().empty() ? gir_module_->get_ast_module().path.string()
                                                     : fn->get_test_file()};
        auto*       line{llvm::ConstantInt::get(types_.get_int32_ty(), fn->get_test_line())};
        auto*       col{llvm::ConstantInt::get(types_.get_int32_ty(), fn->get_test_column())};
        auto*       loc{llvm::ConstantStruct::get(
            loc_struct_ty, {string_slice(file, fmt::format("str.test_file.{}", i)), line, col})};
        descriptors.emplace_back(llvm::ConstantStruct::get(
            test_struct_ty,
            {string_slice(desc, fmt::format("str.test_name.{}", i)), loc, test_llvm_fn}));
    }

    auto* array_ty{llvm::ArrayType::get(test_struct_ty, std::max(test_count, usize{1}))};
    auto* array_init{test_count > 0 ? llvm::ConstantArray::get(array_ty, descriptors)
                                    : llvm::ConstantAggregateZero::get(array_ty)};
    auto* array{new llvm::GlobalVariable(*llvm_module_,
                                         array_ty,
                                         true,
                                         llvm::GlobalValue::InternalLinkage,
                                         array_init,
                                         "__ghoti_tests_data")};
    auto* table{new llvm::GlobalVariable(
        *llvm_module_,
        slice_ty,
        true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantStruct::get(
            slice_ty, {array, llvm::ConstantInt::get(types_.get_usize_ty(), test_count)}),
        "__ghoti_tests_slice")};
    return builder_.CreateLoad(slice_ty, table, "tests.slice");
}

auto llvm_lowering::get_or_create_test_failed_flag() -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (auto* gvar{llvm_module_->getGlobalVariable("__ghoti_test_failed", true)}) { return gvar; }
    return new llvm::GlobalVariable(*llvm_module_,
                                    builder_.getInt1Ty(),
                                    false,
                                    llvm::GlobalValue::InternalLinkage,
                                    builder_.getInt1(false),
                                    "__ghoti_test_failed");
}

auto llvm_lowering::get_or_create_test_skipped_flag() -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (auto* gvar{llvm_module_->getGlobalVariable("__ghoti_test_skipped", true)}) { return gvar; }
    return new llvm::GlobalVariable(*llvm_module_,
                                    builder_.getInt1Ty(),
                                    false,
                                    llvm::GlobalValue::InternalLinkage,
                                    builder_.getInt1(false),
                                    "__ghoti_test_skipped");
}

auto llvm_lowering::get_or_create_test_abort_flag() -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (auto* gvar{llvm_module_->getGlobalVariable("__ghoti_test_aborted", true)}) { return gvar; }
    return new llvm::GlobalVariable(*llvm_module_,
                                    builder_.getInt1Ty(),
                                    false,
                                    llvm::GlobalValue::InternalLinkage,
                                    builder_.getInt1(false),
                                    "__ghoti_test_aborted");
}

auto llvm_lowering::emit_testing_abort_return() -> void {
    auto* ret_ty{builder_.GetInsertBlock()->getParent()->getReturnType()};
    if (ret_ty->isVoidTy()) {
        builder_.CreateRetVoid();
    } else if (lowering_fn_ && lowering_fn_->get_is_test()) {
        // The test's own result folds in the failed flag, so a skip passes and a require fails
        builder_.CreateRet(builder_.getInt1(true));
    } else {
        // The caller returns too without reading the value
        builder_.CreateRet(llvm::PoisonValue::get(ret_ty));
    }
}

auto llvm_lowering::emit_testing_abort_check(const llvm::Function& callee) -> void {
    if (!callee.hasFnAttribute(TESTING_FN_ATTRIBUTE) || !lowering_fn_ ||
        (!lowering_fn_->get_is_test() && !lowering_fn_->get_attributes().testing)) {
        return;
    }
    auto* fn{builder_.GetInsertBlock()->getParent()};
    auto* aborted{builder_.CreateLoad(builder_.getInt1Ty(), get_or_create_test_abort_flag())};
    auto* abort_bb{llvm::BasicBlock::Create(context_, "test.abort", fn)};
    auto* cont_bb{llvm::BasicBlock::Create(context_, "test.continue", fn)};
    builder_.CreateCondBr(aborted, abort_bb, cont_bb);
    builder_.SetInsertPoint(abort_bb);
    emit_testing_abort_return();
    builder_.SetInsertPoint(cont_bb);
}

auto llvm_lowering::define_test_take_skipped() -> void {
    PROFILE_FUNCTION();
    auto* i1_ty{builder_.getInt1Ty()};
    auto* fn{llvm_module_->getFunction("__ghoti_test_take_skipped")};
    if (!fn) {
        auto* fn_ty{llvm::FunctionType::get(i1_ty, {}, false)};
        fn = llvm::Function::Create(fn_ty,
                                    llvm::Function::InternalLinkage,
                                    "__ghoti_test_take_skipped",
                                    llvm_module_.get());
    } else {
        fn->setLinkage(llvm::Function::InternalLinkage);
    }
    if (!fn->empty()) { return; }

    auto*             bb{llvm::BasicBlock::Create(context_, "entry", fn)};
    llvm::IRBuilder<> b{bb};
    auto*             flag{get_or_create_test_skipped_flag()};
    auto*             was_skipped{b.CreateLoad(i1_ty, flag, "skipped")};
    b.CreateStore(b.getInt1(false), flag);
    b.CreateRet(was_skipped);
}

auto llvm_lowering::lower_truthiness(llvm::Value* value) -> llvm::Value* {
    auto* type{value->getType()};
    if (type->isPointerTy()) {
        return builder_.CreateICmpNE(
            value, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(type)), "tobool");
    }
    if (type->isIntegerTy() && type->getIntegerBitWidth() != 1) {
        return builder_.CreateICmpNE(value, llvm::Constant::getNullValue(type), "tobool");
    }
    return value;
}

auto llvm_lowering::branch_to_failure(llvm::Value* cond, std::string_view prefix)
    -> llvm::BasicBlock* {
    auto* cur_fn{builder_.GetInsertBlock()->getParent()};
    auto* fail_bb{llvm::BasicBlock::Create(context_, fmt::format("{}.fail", prefix), cur_fn)};
    auto* cont_bb{llvm::BasicBlock::Create(context_, fmt::format("{}.cont", prefix), cur_fn)};
    builder_.CreateCondBr(cond, cont_bb, fail_bb);
    builder_.SetInsertPoint(fail_bb);
    return cont_bb;
}

auto llvm_lowering::emit_context_handler_call(const gir::instruction& inst,
                                              std::string_view        handler_name,
                                              usize                   msg_idx,
                                              usize                   loc_idx) -> void {
    PROFILE_FUNCTION();
    auto* handler_fn{llvm_module_->getFunction(handler_name)};
    if (!handler_fn) { return; }

    auto* fn_ty{handler_fn->getFunctionType()};
    if (fn_ty->getNumParams() != 2) { return; }
    if (msg_idx >= inst.operands.size() || loc_idx + 2 >= inst.operands.size()) { return; }

    const auto make_arg{[&](const gir::value& op, llvm::Type* param_ty) -> llvm::Value* {
        if (const auto str{op.as_opt<std::string>()}) {
            // A raw string operand (`file`, `msg`) becomes a `[]u8` slice `{ ptr, len }`.
            auto* gstr{builder_.CreateGlobalString(*str, "ch.str")};
            if (param_ty->isStructTy()) {
                llvm::Value* slice{llvm::UndefValue::get(param_ty)};
                slice = builder_.CreateInsertValue(
                    slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                slice = builder_.CreateInsertValue(slice,
                                                   usize_const(str->size()),
                                                   {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
                return slice;
            }
            return gstr;
        }

        auto* v{lower_value(op)};
        if (v && v->getType() != param_ty && v->getType()->isIntegerTy() &&
            param_ty->isIntegerTy()) {
            v = builder_.CreateZExtOrTrunc(v, param_ty);
        }
        return v;
    }};

    auto* msg_arg{make_arg(inst.operands[msg_idx], fn_ty->getParamType(0))};
    if (!msg_arg) { return; }

    auto* loc_ty{llvm::cast<llvm::StructType>(fn_ty->getParamType(1))};
    auto* file_arg{make_arg(inst.operands[loc_idx], loc_ty->getElementType(0))};
    auto* line_v{lower_value(inst.operands[loc_idx + 1])};
    auto* col_v{lower_value(inst.operands[loc_idx + 2])};
    if (!file_arg || !line_v || !col_v) { return; }

    llvm::Value* loc_arg{llvm::UndefValue::get(loc_ty)};
    loc_arg = builder_.CreateInsertValue(loc_arg, file_arg, {0U});
    loc_arg = builder_.CreateInsertValue(loc_arg, line_v, {1U});
    loc_arg = builder_.CreateInsertValue(loc_arg, col_v, {2U});

    builder_.CreateCall(handler_fn, {msg_arg, loc_arg});
}

auto llvm_lowering::emit_lowered_panic(std::string_view message, const gir::instruction& inst)
    -> void {
    PROFILE_FUNCTION();
    auto* panic_fn{llvm_module_->getFunction("panic_handler")};
    if (!panic_fn || panic_fn->getFunctionType()->getNumParams() != 2) {
        auto* trap{
            llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
        builder_.CreateCall(trap, {});
        return;
    }

    const auto make_slice{[&](std::string_view text, llvm::Type* slice_ty) -> llvm::Value* {
        auto*        gstr{builder_.CreateGlobalString(std::string{text}, "panic.str")};
        llvm::Value* slice{llvm::UndefValue::get(slice_ty)};
        slice =
            builder_.CreateInsertValue(slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
        slice = builder_.CreateInsertValue(
            slice, usize_const(text.size()), {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
        return slice;
    }};

    auto* msg_ty{panic_fn->getFunctionType()->getParamType(0)};
    auto* loc_ty{llvm::cast<llvm::StructType>(panic_fn->getFunctionType()->getParamType(1))};

    const auto   loc{inst.location.value_or(source_location{}).one_based()};
    llvm::Value* loc_arg{llvm::UndefValue::get(loc_ty)};
    loc_arg = builder_.CreateInsertValue(
        loc_arg, make_slice(llvm_module_->getName(), loc_ty->getElementType(0)), {0U});
    loc_arg =
        builder_.CreateInsertValue(loc_arg, builder_.getInt32(static_cast<u32>(loc.line)), {1U});
    loc_arg =
        builder_.CreateInsertValue(loc_arg, builder_.getInt32(static_cast<u32>(loc.column)), {2U});

    builder_.CreateCall(panic_fn, {make_slice(message, msg_ty), loc_arg});
}

auto llvm_lowering::emit_arith_guard(llvm::Value*            bad,
                                     std::string_view        message,
                                     const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    // A vector operation fails when any of its lanes does
    if (bad->getType()->isVectorTy()) { bad = builder_.CreateOrReduce(bad); }
    auto* cur_fn{builder_.GetInsertBlock()->getParent()};
    auto* fail_bb{llvm::BasicBlock::Create(context_, "safety.fail", cur_fn)};
    auto* ok_bb{llvm::BasicBlock::Create(context_, "safety.ok", cur_fn)};
    builder_.CreateCondBr(bad, fail_bb, ok_bb);

    builder_.SetInsertPoint(fail_bb);
    emit_lowered_panic(message, inst);
    builder_.CreateUnreachable();

    builder_.SetInsertPoint(ok_bb);
}

auto llvm_lowering::fit_shift_amount(llvm::Value* value, llvm::Value* amount, bool amount_signed)
    -> llvm::Value* {
    auto*      value_ty{value->getType()};
    auto*      amount_ty{amount->getType()};
    const auto width{value_ty->getScalarSizeInBits()};
    if (amount_ty->getScalarSizeInBits() <= width) {
        // A negative amount sign-extends to one no unsigned compare finds in range
        return builder_.CreateIntCast(amount, value_ty, amount_signed, "shamt");
    }
    auto* limit{llvm::ConstantInt::get(amount_ty, width)};
    auto* clamped{builder_.CreateSelect(builder_.CreateICmpUGE(amount, limit), limit, amount)};
    return builder_.CreateTrunc(clamped, value_ty, "shamt");
}

auto llvm_lowering::emit_division_guards(const gir::instruction& inst,
                                         llvm::Value*            lhs,
                                         llvm::Value*            rhs,
                                         bool                    is_signed) -> void {
    auto*      int_ty{lhs->getType()};
    const auto width{int_ty->getScalarSizeInBits()};
    auto*      zero{llvm::ConstantInt::get(int_ty, 0)};
    emit_arith_guard(builder_.CreateICmpEQ(rhs, zero), "integer division by zero", inst);
    if (!is_signed) { return; }
    auto* int_min{llvm::ConstantInt::get(int_ty, llvm::APInt::getSignedMinValue(width))};
    auto* neg_one{llvm::ConstantInt::getSigned(int_ty, -1)};
    auto* edge{builder_.CreateAnd(builder_.CreateICmpEQ(lhs, int_min),
                                  builder_.CreateICmpEQ(rhs, neg_one))};
    emit_arith_guard(edge, "signed division overflow", inst);
}

auto llvm_lowering::emit_checked_arith(const gir::instruction& inst,
                                       llvm::Value*            lhs,
                                       llvm::Value*            rhs,
                                       bool                    is_signed) -> llvm::Value* {
    PROFILE_FUNCTION();
    auto* int_ty{lhs->getType()};
    if (!int_ty->isIntOrIntVectorTy()) { return nullptr; }
    const auto width{int_ty->getScalarSizeInBits()};

    switch (inst.kind) {
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL: {
        if (!is_signed) { return nullptr; } // unsigned wrap is defined
        const auto id{inst.kind == gir::instruction_kind::ADD ? llvm::Intrinsic::sadd_with_overflow
                      : inst.kind == gir::instruction_kind::SUB
                          ? llvm::Intrinsic::ssub_with_overflow
                          : llvm::Intrinsic::smul_with_overflow};
        auto*      fn{llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, {int_ty})};
        auto*      pair{builder_.CreateCall(fn, {lhs, rhs})};
        auto*      overflow{builder_.CreateExtractValue(pair, {1U})};

        std::string_view msg;
        if (inst.kind == gir::instruction_kind::ADD) {
            msg = "signed addition overflow";
        } else if (inst.kind == gir::instruction_kind::SUB) {
            msg = "signed subtraction overflow";
        } else {
            msg = "signed multiplication overflow";
        }
        emit_arith_guard(overflow, msg, inst);
        return builder_.CreateExtractValue(pair, {0U});
    }
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD:
        emit_division_guards(inst, lhs, rhs, is_signed);
        return nullptr;
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR: {
        auto* limit{llvm::ConstantInt::get(rhs->getType(), width)};
        emit_arith_guard(builder_.CreateICmpUGE(rhs, limit), "shift amount out of range", inst);
        return nullptr;
    }
    default: return nullptr;
    }
}

auto llvm_lowering::emit_float_to_int_guard(const gir::instruction& inst,
                                            llvm::Value*            val,
                                            llvm::Type*             int_ty,
                                            bool                    is_signed) -> void {
    // Truncation fits iff MIN - 1 < val < MAX + 1. The upper bound is a power of two (exact or
    // infinite); the lower one rounds down, which admits no extra float. NaN fails both checks.
    const auto  width{int_ty->getIntegerBitWidth()};
    const auto& semantics{val->getType()->getFltSemantics()};
    const auto  to_float = [&](const llvm::APInt& bound, llvm::APFloat::roundingMode mode) {
        llvm::APFloat f{semantics};
        DISCARD(f.convertFromAPInt(bound, true, mode));
        return llvm::ConstantFP::get(val->getType(), f);
    };

    const auto min{is_signed ? llvm::APInt::getSignedMinValue(width).sext(width + 2)
                             : llvm::APInt{width + 2, 0}};
    auto*      lo{to_float(min - 1, llvm::APFloat::rmTowardNegative)};
    auto*      hi{to_float(llvm::APInt::getOneBitSet(width + 2, is_signed ? width - 1 : width),
                           llvm::APFloat::rmNearestTiesToEven)};

    auto* in_range{
        builder_.CreateAnd(builder_.CreateFCmpOGT(val, lo), builder_.CreateFCmpOLT(val, hi))};
    emit_arith_guard(
        builder_.CreateNot(in_range), "float value out of range for integer cast", inst);
}

auto llvm_lowering::emit_saturating_arith(const gir::instruction& inst,
                                          llvm::Value*            lhs,
                                          llvm::Value*            rhs,
                                          bool                    is_signed) -> llvm::Value* {
    PROFILE_FUNCTION();
    auto* int_ty{lhs->getType()};
    switch (inst.kind) {
    case gir::instruction_kind::ADD:
        return builder_.CreateBinaryIntrinsic(
            is_signed ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat, lhs, rhs);
    case gir::instruction_kind::SUB:
        return builder_.CreateBinaryIntrinsic(
            is_signed ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat, lhs, rhs);
    case gir::instruction_kind::MUL: {
        // There's no plain `mul.sat`, but a fixed-point multiply with scale 0 is exactly that
        const auto id{is_signed ? llvm::Intrinsic::smul_fix_sat : llvm::Intrinsic::umul_fix_sat};
        auto*      fn{llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, {int_ty})};
        return builder_.CreateCall(fn, {lhs, rhs, builder_.getInt32(0)}, "mulsat");
    }
    case gir::instruction_kind::SHL: {
        // `shl.sat` is poison once the amount reaches the width, where any non-zero lhs saturates
        const auto width{int_ty->getScalarSizeInBits()};
        auto*      zero{llvm::ConstantInt::get(int_ty, 0)};
        auto*      in_range{builder_.CreateICmpULT(rhs, llvm::ConstantInt::get(int_ty, width))};
        auto*      amount{builder_.CreateSelect(in_range, rhs, zero)};
        auto*      shifted{builder_.CreateBinaryIntrinsic(
            is_signed ? llvm::Intrinsic::sshl_sat : llvm::Intrinsic::ushl_sat, lhs, amount)};

        llvm::Value* limit{llvm::ConstantInt::get(int_ty, llvm::APInt::getMaxValue(width))};
        if (is_signed) {
            limit = builder_.CreateSelect(
                builder_.CreateICmpSLT(lhs, zero),
                llvm::ConstantInt::get(int_ty, llvm::APInt::getSignedMinValue(width)),
                llvm::ConstantInt::get(int_ty, llvm::APInt::getSignedMaxValue(width)));
        }
        auto* saturated{builder_.CreateSelect(builder_.CreateICmpEQ(lhs, zero), zero, limit)};
        return builder_.CreateSelect(in_range, shifted, saturated, "shlsat");
    }
    default: UNREACHABLE("Only + - * << have saturating forms");
    }
}

auto llvm_lowering::const_callable(std::string_view          fn_symbol,
                                   stdx::option<sema::type&> fn_type,
                                   llvm::Type*               ty) -> llvm::Constant* {
    auto* st{llvm::dyn_cast<llvm::StructType>(ty)};
    if (!st || st->getNumElements() != 2 || !st->getElementType(1)->isPointerTy()) {
        return nullptr;
    }
    const auto fn{fn_type ? fn_type->get_data().as_opt<sema::types::function>() : stdx::none};
    if (!fn || fn->erased) { return nullptr; }
    auto* code_ptr{resolve_named_function(fn_symbol)};
    if (!code_ptr) { return nullptr; }
    return llvm::ConstantStruct::get(st, {code_ptr, get_or_create_fn_trampoline(*fn)});
}

auto llvm_lowering::const_to_llvm(const gir::const_value& cv, llvm::Type* ty) -> llvm::Constant* {
    PROFILE_FUNCTION();
    if (!ty) { return nullptr; }
    if (const auto i{cv.as_opt<i64>()}) { return int_or_ptr_constant(static_cast<u128>(*i), ty); }
    if (const auto u{cv.as_opt<u64>()}) { return int_or_ptr_constant(*u, ty); }
    if (const auto w{cv.as_opt<i128>()}) { return int_or_ptr_constant(static_cast<u128>(*w), ty); }
    if (const auto w{cv.as_opt<u128>()}) { return int_or_ptr_constant(*w, ty); }
    if (const auto f{cv.as_opt<f128>()}) { return float_constant(*f, ty); }
    if (const auto b{cv.as_opt<bool>()}) { return llvm::ConstantInt::getBool(context_, *b); }
    if (const auto e{cv.as_opt<gir::const_enum>()}) {
        return llvm::ConstantInt::get(ty, static_cast<u64>(e->value), true);
    }
    if (const auto s{cv.as_opt<std::string>()}) {
        if (auto* callable{const_callable(*s, cv.get_type(), ty)}) { return callable; }
        if (auto* st{llvm::dyn_cast<llvm::StructType>(ty)}) {
            if (st->getNumElements() == 2 && st->getElementType(0)->isPointerTy() &&
                st->getElementType(1)->isIntegerTy()) {
                const auto slice_data{cv.get_type()
                                          ? cv.get_type()->get_data().as_opt<sema::types::slice>()
                                          : stdx::none};
                const bool add_nul{slice_data && slice_data->null_terminated};
                auto*      data{llvm::ConstantDataArray::getString(context_, *s, add_nul)};
                auto*      data_gv{new llvm::GlobalVariable{*llvm_module_,
                                                            data->getType(),
                                                            true,
                                                            llvm::GlobalValue::PrivateLinkage,
                                                            data,
                                                            ".str.data"}};
                data_gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                auto* len{llvm::ConstantInt::get(st->getElementType(1), s->size())};
                return llvm::ConstantStruct::get(st, {data_gv, len});
            }
        }
        if (ty->isPointerTy()) {
            if (auto* c{resolve_named_constant(*s)}) { return c; }
            // Pass the module explicitly: `const_to_llvm` also runs while lowering module
            // globals, where the IRBuilder has no insertion block to source it from.
            return builder_.CreateGlobalString(*s, "gstr", 0, llvm_module_.get());
        }
        if (auto* at{llvm::dyn_cast<llvm::ArrayType>(ty)}) {
            std::string bytes{*s};
            bytes.resize(static_cast<usize>(at->getNumElements()), '\0');
            return llvm::ConstantDataArray::getString(context_, bytes, false);
        }
        return llvm::Constant::getNullValue(ty);
    }
    if (const auto addr{cv.as_opt<gir::const_addr>()}) {
        if (ty->isPointerTy()) {
            if (auto* c{resolve_named_constant(addr->symbol)}) { return c; }
        }
        return llvm::Constant::getNullValue(ty);
    }
    if (const auto dyn{cv.as_opt<gir::const_dyn_fat_ptr>()}) {
        auto* st{llvm::dyn_cast<llvm::StructType>(ty)};
        if (!st || st->getNumElements() != 2) { return llvm::Constant::getNullValue(ty); }
        auto* ptr_ty{types_.get_ptr_ty()};
        auto* data_c{dyn->data_symbol.empty() ? llvm::ConstantPointerNull::get(ptr_ty)
                                              : resolve_named_constant(dyn->data_symbol)};
        auto* vt_c{dyn->vtable_symbol.empty() ? llvm::ConstantPointerNull::get(ptr_ty)
                                              : resolve_named_constant(dyn->vtable_symbol)};
        if (!data_c) { data_c = llvm::ConstantPointerNull::get(ptr_ty); }
        if (!vt_c) { vt_c = llvm::ConstantPointerNull::get(ptr_ty); }
        return llvm::ConstantStruct::get(st, {data_c, vt_c});
    }
    if (cv.is<gir::nullptr_val>()) { return llvm::Constant::getNullValue(ty); }
    if (const auto un{cv.as_opt<gir::const_union>()}) {
        const auto sema_ty{cv.get_type()};
        const auto ud{sema_ty ? sema_ty->get_data().as_opt<sema::types::union_t>() : stdx::none};
        if (!ud) { return llvm::Constant::getNullValue(ty); }

        u32 active_idx{0};
        for (usize i{0}; i < ud->fields.size(); ++i) {
            const auto& fname{
                ud->enclosing.ast.get_as<ast::identifier_expr>(ud->ast_fields[i].name).name};
            if (fname == un->active_field) {
                active_idx = static_cast<u32>(i);
                break;
            }
        }

        if (ud->is_bit_packed()) {
            if (!un->payload.empty()) {
                if (const auto int_val{un->payload.front().as_int_opt()}) {
                    return llvm::ConstantInt::get(ty, static_cast<u64>(*int_val));
                }
            }
            return llvm::Constant::getNullValue(ty);
        }

        if (ud->is_untagged) {
            auto* st{llvm::dyn_cast<llvm::StructType>(ty)};
            if (!st || un->payload.empty() || active_idx >= ud->fields.size()) {
                return llvm::Constant::getNullValue(ty);
            }
            const auto& payload{un->payload.front()};
            auto*       member_ty{types_.translate(*ud->fields[active_idx])};
            auto*       member{const_to_llvm(payload, member_ty)};
            if (member_ty == st->getElementType(0)) {
                std::vector<llvm::Constant*> elems{member};
                if (st->getNumElements() > 1) {
                    elems.emplace_back(llvm::Constant::getNullValue(st->getElementType(1)));
                }
                return llvm::ConstantStruct::get(st, elems);
            }
            return reinterpret_constant(member, ty, 0, llvm_module_->getDataLayout());
        }

        auto* st{llvm::dyn_cast<llvm::StructType>(ty)};
        if (!st) { return llvm::Constant::getNullValue(ty); }

        auto* tag_c{llvm::ConstantInt::get(st->getElementType(0), active_idx)};
        if (st->getNumElements() == 1) { return llvm::ConstantStruct::get(st, {tag_c}); }

        auto* payload_arr_ty{llvm::dyn_cast<llvm::ArrayType>(st->getElementType(1))};
        if (!payload_arr_ty) {
            return llvm::ConstantStruct::get(
                st, {tag_c, llvm::Constant::getNullValue(st->getElementType(1))});
        }

        const auto bytes{
            un->payload.empty()
                ? std::string(payload_arr_ty->getNumElements(), '\0')
                : union_payload_bytes(un->payload.front(), payload_arr_ty->getNumElements())};
        auto* payload_c{llvm::ConstantDataArray::getString(context_, bytes, false)};
        return llvm::ConstantStruct::get(st, {tag_c, payload_c});
    }
    if (const auto st{cv.as_opt<gir::const_struct>()}) {
        auto*      llvm_st{llvm::dyn_cast<llvm::StructType>(ty)};
        const auto sema_ty{cv.get_type()};
        const auto sd{sema_ty ? sema_ty->get_data().as_opt<sema::types::struct_t>() : stdx::none};
        if (!llvm_st || !sd) { return llvm::Constant::getNullValue(ty); }
        std::vector<llvm::Constant*> elems;
        elems.reserve(llvm_st->getNumElements());
        for (auto* elem_ty : llvm_st->elements()) {
            elems.emplace_back(llvm::Constant::getNullValue(elem_ty));
        }
        for (usize i{0}; i < sd->fields.size(); ++i) {
            const auto elem_idx{types_.struct_field_index(llvm_st, static_cast<u32>(i))};
            if (elem_idx >= elems.size()) { break; }
            const auto& fname{
                sd->enclosing.ast.get_as<ast::identifier_expr>(sd->ast_fields[i].name).name};
            if (const auto fv{st->get_field_opt(fname)}) {
                elems[elem_idx] = const_to_llvm(*fv, llvm_st->getElementType(elem_idx));
            }
        }
        return llvm::ConstantStruct::get(llvm_st, elems);
    }
    if (const auto arr{cv.as_opt<gir::const_array>()}) {
        if (auto* vt{llvm::dyn_cast<llvm::FixedVectorType>(ty)}) {
            std::vector<llvm::Constant*> lanes;
            lanes.reserve(vt->getNumElements());
            for (const auto& lane : arr->elements) {
                lanes.emplace_back(const_to_llvm(lane, vt->getElementType()));
            }
            while (lanes.size() < vt->getNumElements()) {
                lanes.emplace_back(llvm::Constant::getNullValue(vt->getElementType()));
            }
            return llvm::ConstantVector::get(lanes);
        }
        auto* at{llvm::dyn_cast<llvm::ArrayType>(ty)};
        if (!at) { return llvm::Constant::getNullValue(ty); }
        auto*                        elem_ty{at->getElementType()};
        std::vector<llvm::Constant*> elems;
        elems.reserve(static_cast<usize>(at->getNumElements()));
        for (const auto& elem : arr->elements) { elems.emplace_back(const_to_llvm(elem, elem_ty)); }
        while (elems.size() < at->getNumElements()) {
            elems.emplace_back(llvm::Constant::getNullValue(elem_ty));
        }
        return llvm::ConstantArray::get(at, elems);
    }
    return llvm::Constant::getNullValue(ty);
}

auto llvm_lowering::lower_global(const gir::global_decl& g) -> llvm::GlobalVariable* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(g.name)}; it != globals_.end()) {
        if (auto* existing{llvm::dyn_cast<llvm::GlobalVariable>(it->second)}) { return existing; }
    }
    if (auto* existing{llvm_module_->getGlobalVariable(g.name)}) { return existing; }
    ensure_reserved_symbols();

    auto* g_type{types_.translate(g.type)};
    // A `void` global has no storage to emit
    if (g_type->isVoidTy() || g_type->isFunctionTy()) { return nullptr; }
    const bool is_const{g.is_constant};
    auto g_linkage{(g.linkage == gir::linkage::INTERNAL) ? llvm::GlobalValue::InternalLinkage
                                                         : llvm::GlobalValue::ExternalLinkage};
    if (g.is_weak && g_linkage != llvm::GlobalValue::InternalLinkage) {
        g_linkage = (g.linkage == gir::linkage::EXTERN) ? llvm::GlobalValue::ExternalWeakLinkage
                                                        : llvm::GlobalValue::WeakAnyLinkage;
    }

    std::string sym_name{g.link_name.empty() ? g.name : g.link_name};
    // A plain (INTERNAL/PUBLIC) ghoti global whose name collides with a C-ABI symbol
    if (g.link_name.empty() && !g.is_weak &&
        (llvm_module_->getNamedValue(sym_name) || reserved_symbols_.contains(sym_name))) {
        sym_name  = private_symbol_name(sym_name);
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }
    auto* gvar{
        new llvm::GlobalVariable{*llvm_module_, g_type, is_const, g_linkage, nullptr, sym_name}};
    if (g.is_thread_local) {
        gvar->setThreadLocalMode(is_executable_ ? llvm::GlobalValue::LocalExecTLSModel
                                                : llvm::GlobalValue::GeneralDynamicTLSModel);
    }
    const auto wanted_alignment{
        std::max(g.alignment.value_or(0), types_.explicit_alignment_of(g.type).value_or(0))};
    if (wanted_alignment > 0) {
        const auto natural{llvm_module_->getDataLayout().getPrefTypeAlign(g_type)};
        gvar->setAlignment(std::max(natural, llvm::Align{wanted_alignment}));
    }
    apply_visibility(*gvar, g.visibility);
    globals_[g.name] = gvar;

    llvm::Constant* init{nullptr};
    if (g.const_init) {
        init = const_to_llvm(*g.const_init, g_type);
    } else if (g.init_value && g.init_value->is<std::string>() &&
               g.type.get_kind() == sema::type_kind::SLICE) {
        // The folded value of a comptime str slice is a bare str, but the global's type is a slice
        const auto& bytes{g.init_value->as<std::string>()};
        const auto  slice_data{g.type.get_data().as_opt<sema::types::slice>()};
        const bool  add_nul{slice_data && slice_data->null_terminated};
        auto*       data{llvm::ConstantDataArray::getString(context_, bytes, add_nul)};
        auto*       data_gv{new llvm::GlobalVariable{*llvm_module_,
                                                     data->getType(),
                                                     true,
                                                     llvm::GlobalValue::PrivateLinkage,
                                                     data,
                                                     ".str.data"}};
        data_gv->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        auto* len{llvm::ConstantInt::get(types_.get_usize_ty(), bytes.size())};
        init = llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(g_type), {data_gv, len});
    } else if (g.init_value) {
        if (const auto fn_symbol{g.init_value->as_opt<std::string>()}) {
            init = const_callable(*fn_symbol, g.init_value->type, g_type);
        }
        auto* init_v{init ? nullptr : lower_value(*g.init_value, g.type)};
        if (init_v && llvm::isa<llvm::Constant>(init_v)) {
            init = llvm::cast<llvm::Constant>(init_v);
        }
    }

    if (!init && g.linkage != gir::linkage::EXTERN) { init = llvm::Constant::getNullValue(g_type); }
    if (init) { gvar->setInitializer(init); }

    return gvar;
}

auto llvm_lowering::ensure_reserved_symbols() -> void {
    PROFILE_FUNCTION();
    if (reserved_symbols_built_) { return; }
    reserved_symbols_built_ = true;
    // The synthesized C entry point owns `main` in any linked executable
    if (is_executable_) { reserved_symbols_.emplace("main"); }
    if (!gir_module_) { return; }
    for (const auto f : gir_module_->get_functions()) {
        if (!f->get_link_name().empty()) { reserved_symbols_.emplace(f->get_link_name()); }
    }
    for (const auto g : gir_module_->get_globals()) {
        if (!g->link_name.empty()) { reserved_symbols_.emplace(g->link_name); }
    }
}

auto llvm_lowering::private_symbol_name(std::string_view name) const -> std::string {
    auto candidate{fmt::format("__ghoti.{}", name)};
    for (u32 n{1}; llvm_module_->getNamedValue(candidate) != nullptr; ++n) {
        candidate = fmt::format("__ghoti.{}.{}", name, n);
    }
    return candidate;
}

auto llvm_lowering::declare_function(const gir::function& fn) -> llvm::Function* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(fn.get_name())}; it != globals_.end()) {
        if (auto* existing{llvm::dyn_cast<llvm::Function>(it->second)}) { return existing; }
    }
    ensure_reserved_symbols();

    const auto fn_data{sema::signature_of(fn.get_type())};
    ASSERT(fn_data, "Function must have function sema type");
    auto* fn_ty{types_.translate_function_type(*fn_data)};

    std::string fn_name{fn.get_name()};
    auto        g_linkage{llvm::GlobalValue::ExternalLinkage};
    if (fn.get_linkage() == gir::linkage::INTERNAL) {
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    // Whether this function's symbol name is externally meaningful and must be kept verbatim
    bool fixed_symbol{false};
    if (is_executable_ && fn_name == user_main_name_) {
        fn_name      = "_ghoti_main";
        g_linkage    = llvm::GlobalValue::InternalLinkage;
        fixed_symbol = true;
    } else if (!fn.get_link_name().empty()) {
        fn_name      = std::string{fn.get_link_name()};
        fixed_symbol = true;
    }

    if (fn.get_is_weak() && g_linkage != llvm::GlobalValue::InternalLinkage) {
        g_linkage    = (fn.get_linkage() == gir::linkage::EXTERN)
                           ? llvm::GlobalValue::ExternalWeakLinkage
                           : llvm::GlobalValue::WeakAnyLinkage;
        fixed_symbol = true;
    }

    auto* existing{llvm_module_->getFunction(fn_name)};
    if (existing && (fixed_symbol || existing->getFunctionType() == fn_ty)) { return existing; }

    // An internal/pub ghoti definition whose natural name collides with a C-ABI symbol
    if (!fixed_symbol && (existing != nullptr || reserved_symbols_.contains(fn_name))) {
        fn_name   = private_symbol_name(fn_name);
        g_linkage = llvm::GlobalValue::InternalLinkage;
    }

    auto* llvm_fn{llvm::Function::Create(fn_ty, g_linkage, fn_name, llvm_module_.get())};
    llvm_fn->setAttributes(with_integer_extensions(llvm_fn->getAttributes(), *fn_data));
    llvm_fn->addFnAttr(llvm::Attribute::NoBuiltin);
    // Non x86-64 Windows links no runtime that could provide a probe symbol
    if (!windows_stack_probe_symbol()) { llvm_fn->addFnAttr("no-stack-arg-probe", "true"); }
    if (fn.get_calling_conv() != ast::calling_convention::C) {
        llvm_fn->setCallingConv(
            to_llvm_callconv(fn.get_calling_conv(), llvm::Triple{llvm_module_->getTargetTriple()}));
    }
    apply_fn_attributes(*llvm_fn, fn.get_attributes());
    for (usize arg_idx{0}; const auto& param : fn.get_params()) {
        auto* p_ty{types_.translate(param->type)};
        if (p_ty->isVoidTy()) { continue; }
        auto* arg{llvm_fn->getArg(static_cast<u32>(arg_idx++))};
        arg->setName(param->name);
    }

    // Every further `@export` name is another symbol for the same code
    for (const auto& alias : fn.get_aliases()) {
        auto* symbol{llvm::GlobalAlias::create(fn_ty,
                                               0,
                                               alias.weak ? llvm::GlobalValue::WeakAnyLinkage
                                                          : llvm::GlobalValue::ExternalLinkage,
                                               alias.name,
                                               llvm_fn,
                                               llvm_module_.get())};
        apply_visibility(*symbol, alias.visibility);
    }

    if (fn.get_attributes().testing) { llvm_fn->addFnAttr(TESTING_FN_ATTRIBUTE); }

    globals_[fn.get_name()] = llvm_fn;
    return llvm_fn;
}

auto llvm_lowering::lower_function(const gir::function& fn) -> llvm::Function* {
    PROFILE_FUNCTION();
    auto* llvm_fn{declare_function(fn)};
    if (!llvm_fn) { return llvm_fn; }
    const auto fn_data{sema::signature_of(fn.get_type())};
    if (fn.get_linkage() == gir::linkage::EXTERN || fn.get_segments().empty() ||
        (fn_data && is_comptime_only_signature(*fn_data))) {
        return llvm_fn;
    }
    clear_locals();
    lowering_fn_.emplace(fn);

    // Pre-allocate basic blocks for all segments
    for (const auto seg : fn.get_segments()) {
        auto* bb{llvm::BasicBlock::Create(
            context_, fmt::format("seg_{}", std::to_underlying(seg->get_id())), llvm_fn)};
        segment_blocks_[seg->get_id()] = bb;
    }

    // Bind parameters; spilled slots (if any) go at the top of the entry block
    builder_.SetInsertPoint(segment_blocks_[fn.get_segments()[0]->get_id()]);
    for (usize arg_idx{0}; const auto& param : fn.get_params()) {
        auto* p_ty{types_.translate(param->type)};
        if (p_ty->isVoidTy()) {
            // Void params have no LLVM argument but are still referenced like void loads
            set_local(param->id, nullptr);
            continue;
        }
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

    if (fn.get_is_test()) {
        builder_.CreateStore(builder_.getInt1(false), get_or_create_test_failed_flag());
        builder_.CreateStore(builder_.getInt1(false), get_or_create_test_skipped_flag());
        builder_.CreateStore(builder_.getInt1(false), get_or_create_test_abort_flag());
    }

    // Lower each segment; dead code after a diverging expression was never type checked
    const auto reachable{fn.reachable_segments()};
    for (const auto seg : fn.get_segments()) {
        auto* bb{segment_blocks_[seg->get_id()]};
        builder_.SetInsertPoint(bb);
        if (const auto idx{std::to_underlying(seg->get_id())};
            idx < reachable.size() && !reachable[idx]) {
            builder_.CreateUnreachable();
            continue;
        }

        for (const auto inst : seg->get_instructions()) { lower_instruction(*inst); }
        if (!bb->getTerminator()) {
            gsl::not_null<const sema::type*> target_ret{&fn.get_type()};
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

    if (fn.get_is_test()) {
        auto*                          flag{get_or_create_test_failed_flag()};
        std::vector<llvm::ReturnInst*> rets;
        for (auto& bb : *llvm_fn) {
            if (auto* ret{llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())}) {
                if (auto* rv{ret->getReturnValue()}; rv && rv->getType()->isIntegerTy(1)) {
                    rets.emplace_back(ret);
                }
            }
        }
        for (auto* ret : rets) {
            llvm::IRBuilder<> rb{ret};
            auto*             failed{rb.CreateLoad(rb.getInt1Ty(), flag, "test.failed")};
            auto*             combined{
                rb.CreateAnd(ret->getReturnValue(), rb.CreateNot(failed), "test.result")};
            rb.CreateRet(combined);
            ret->eraseFromParent();
        }
    }

    return llvm_fn;
}

auto llvm_lowering::lower_value(const gir::value&               val,
                                stdx::option<const sema::type&> expected_type) -> llvm::Value* {
    PROFILE_FUNCTION();
    return val.data.visit(
        [this](gir::local_id loc) -> llvm::Value* {
            const auto it{locals_.find(loc)};
            ASSERT(it != locals_.end(), "Referenced GIR local was never lowered");
            return it->second;
        },
        [this, &val, expected_type](i64 i) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? (val.type->get_kind() == sema::type_kind::COMPTIME_INT &&
                                              (i < std::numeric_limits<i32>::min() ||
                                               i > std::numeric_limits<i32>::max())
                                          ? types_.get_int64_ty()
                                          : types_.translate(*val.type))
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(static_cast<u128>(i), ty);
        },
        [this, &val, expected_type](u64 u) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? (val.type->get_kind() == sema::type_kind::COMPTIME_INT &&
                                              u > std::numeric_limits<u32>::max()
                                          ? types_.get_int64_ty()
                                          : types_.translate(*val.type))
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(u, ty);
        },
        [this, &val, expected_type](i128 w) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type
                         ? (val.type->get_kind() == sema::type_kind::COMPTIME_INT &&
                                    (w < static_cast<i128>(std::numeric_limits<i32>::min()) ||
                                     w > static_cast<i128>(std::numeric_limits<i32>::max()))
                                ? (w < static_cast<i128>(std::numeric_limits<i64>::min()) ||
                                           w > static_cast<i128>(std::numeric_limits<i64>::max())
                                       ? llvm::Type::getInt128Ty(context_)
                                       : types_.get_int64_ty())
                                : types_.translate(*val.type))
                         : types_.get_int64_ty()};
            return int_or_ptr_constant(static_cast<u128>(w), ty);
        },
        [this, &val, expected_type](u128 w) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? (val.type->get_kind() == sema::type_kind::COMPTIME_INT &&
                                              w > std::numeric_limits<u32>::max()
                                          ? (w > std::numeric_limits<u64>::max()
                                                 ? llvm::Type::getInt128Ty(context_)
                                                 : types_.get_int64_ty())
                                          : types_.translate(*val.type))
                                   : types_.get_int64_ty()};
            return int_or_ptr_constant(w, ty);
        },
        [this, &val, expected_type](const big_int& b) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : nullptr};
            if (!ty || !ty->isIntegerTy()) {
                ty = llvm::IntegerType::get(context_, static_cast<u32>(b.signed_bits()));
            }
            const auto width{ty->getIntegerBitWidth()};
            const auto words{b.to_words(width)};
            return llvm::ConstantInt::get(context_, llvm::APInt(width, words));
        },
        [this, &val, expected_type](f128 f) -> llvm::Value* {
            auto* ty{expected_type ? types_.translate(*expected_type)
                     : val.type    ? types_.translate(*val.type)
                                   : types_.get_double_ty()};
            return float_constant(f, ty);
        },
        [this](bool b) -> llvm::Value* { return llvm::ConstantInt::getBool(context_, b); },
        [this, &val, expected_type](const std::string& str) -> llvm::Value* {
            auto ty{expected_type};
            if (!ty && val.type) { ty = val.type; }
            // A fn-typed string value names a function rather than holding string data, even when
            // stored into a `^fn(...)` slot (that pointer is the function's own address)
            if (val.type && val.type->get_kind() == sema::type_kind::FUNCTION) {
                ty = val.type;
            } else if (const auto p{ty ? ty->get_data().as_opt<sema::types::pointer>()
                                       : stdx::none};
                       p && p->underlying.get_kind() == sema::type_kind::FUNCTION) {
                ty = p->underlying;
            }
            if (ty && ty->get_kind() == sema::type_kind::FUNCTION) {
                if (auto* fn{resolve_named_function(str)}) { return fn; }
                // Not yet declared in this translation unit for the linnker to resolve
                const auto fn_data{ty->get_data().as_opt<sema::types::function>()};
                ASSERT(fn_data, "FUNCTION-typed value must carry function type data");
                auto* fn_ty{types_.translate_function_type(*fn_data)};
                return llvm::Function::Create(
                    fn_ty, llvm::Function::ExternalLinkage, str, llvm_module_.get());
            }
            if (ty && ty->get_kind() == sema::type_kind::ARRAY) {
                return llvm::ConstantDataArray::getString(context_, str, true);
            }
            // Give `CreateGlobalString` the module directly
            return builder_.CreateGlobalString(str, "str", 0, llvm_module_.get());
        },
        [this, &val, expected_type](gir::undefined_val) -> llvm::Value* {
            // A bare `undefined` may still carry the placeholder UNDEFINED/POISON type; fall back
            // to the expected type.
            const auto usable{[](stdx::option<const sema::type&> t) {
                return t && !t->is_poison() && t->get_kind() != sema::type_kind::UNDEFINED;
            }};
            const auto ty{usable(val.type)        ? val.type
                          : usable(expected_type) ? expected_type
                                                  : stdx::none};
            auto*      llty{ty ? types_.translate(*ty) : types_.get_int64_ty()};
            if (llty->isVoidTy()) { return nullptr; }
            return llvm::UndefValue::get(llty);
        },
        [this, &val, expected_type](gir::zero_val) -> llvm::Value* {
            const auto ty{expected_type ? stdx::option<const sema::type&>{expected_type}
                                        : val.type};
            auto*      llty{ty ? types_.translate(*ty) : types_.get_int64_ty()};
            if (llty->isVoidTy()) { return nullptr; }
            return llvm::Constant::getNullValue(llty);
        },
        [](gir::void_val) -> llvm::Value* { return nullptr; },
        [this](gir::nullptr_val) -> llvm::Value* {
            return llvm::ConstantPointerNull::get(types_.get_ptr_ty());
        },
        [](stdx::option<sema::type&>) -> llvm::Value* { return nullptr; });
}

auto llvm_lowering::lower_instruction(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    llvm::Value* result_val{nullptr};

    switch (inst.kind) {
    case gir::instruction_kind::ALLOCA:          result_val = emit_alloca(inst); break;
    case gir::instruction_kind::LOAD:            result_val = emit_load(inst); break;
    case gir::instruction_kind::STORE:           emit_store(inst); return;
    case gir::instruction_kind::GET_ELEMENT_PTR: result_val = emit_get_element_ptr(inst); break;
    case gir::instruction_kind::ADDRESS_OF:      result_val = emit_address_of(inst); break;
    case gir::instruction_kind::DEREF:           result_val = emit_deref(inst); break;
    case gir::instruction_kind::GLOBAL_ADDR:     result_val = emit_global_addr(inst); break;
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
    case gir::instruction_kind::INT_CAST:
    case gir::instruction_kind::TRUNC_CAST:
    case gir::instruction_kind::BIT_CAST:
    case gir::instruction_kind::PTR_CAST:
    case gir::instruction_kind::INT_FROM_PTR:
    case gir::instruction_kind::PTR_FROM_INT:    result_val = emit_cast(inst); break;
    case gir::instruction_kind::MAKE_CALLABLE:   result_val = emit_make_callable(inst); break;
    case gir::instruction_kind::SPLAT:
    case gir::instruction_kind::EXTRACT_LANE:
    case gir::instruction_kind::INSERT_LANE:
    case gir::instruction_kind::REPACK:
    case gir::instruction_kind::SHUFFLE:
    case gir::instruction_kind::SELECT:
    case gir::instruction_kind::REDUCE:          result_val = emit_vector_op(inst); break;
    case gir::instruction_kind::CALL:            {
        auto*              block{builder_.GetInsertBlock()};
        llvm::Instruction* before{block && !block->empty() ? &block->back() : nullptr};
        result_val = emit_call(inst);
        if (inst.call_modifier && block) {
            apply_call_modifier(*inst.call_modifier, *block, before);
        }
        break;
    }
    case gir::instruction_kind::BUILTIN_CALL: result_val = emit_builtin_call(inst); break;
    case gir::instruction_kind::INLINE_ASM:   result_val = emit_inline_asm(inst); break;
    case gir::instruction_kind::RET:          emit_ret(inst); return;
    case gir::instruction_kind::GOTO:         emit_goto(inst); return;
    case gir::instruction_kind::COND_GOTO:    emit_cond_goto(inst); return;
    case gir::instruction_kind::UNREACHABLE:  emit_unreachable(); return;
    }

    if (result_val && inst.result) { set_local(*inst.result, result_val); }
}

auto llvm_lowering::emit_alloca(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.type, "Alloca requires a type");
    auto* elem_ty{types_.translate(*inst.type)};
    if (elem_ty->isVoidTy()) {
        auto* dummy{llvm::UndefValue::get(types_.get_ptr_ty())};
        if (inst.result) { set_local(*inst.result, dummy); }
        return dummy;
    }

    // Every slot lives in the entry block: one placed where a loop body declares it would take
    // fresh stack on each iteration
    auto&             entry{builder_.GetInsertBlock()->getParent()->getEntryBlock()};
    llvm::IRBuilder<> entry_builder{&entry, entry.getFirstInsertionPt()};
    auto*             slot{entry_builder.CreateAlloca(elem_ty, nullptr, "slot")};
    const auto        wanted{
        std::max(inst.alignment.value_or(0), types_.explicit_alignment_of(*inst.type).value_or(0))};
    if (wanted > slot->getAlign().value()) { slot->setAlignment(llvm::Align{wanted}); }
    if (inst.result) { set_local(*inst.result, slot); }
    return slot;
}

auto llvm_lowering::emit_load(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
    // Either `store val, dest` or a result slot initialized from its single operand
    const bool has_explicit_dest{inst.operands.size() >= 2};
    if (!has_explicit_dest && (!inst.result || inst.operands.empty())) { return; }

    auto* dest_ptr{has_explicit_dest ? lower_value(inst.operands[0])
                                     : lower_value(gir::value{*inst.result})};
    auto* val{lower_value(inst.operands[has_explicit_dest ? 1 : 0], inst.type)};
    if (!dest_ptr || !dest_ptr->getType()->isPointerTy() || !val || val->getType()->isVoidTy()) {
        return;
    }
    builder_.CreateStore(val, dest_ptr, inst.is_volatile());
}

auto llvm_lowering::emit_get_element_ptr(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "GEP requires base and at least one index");
    // A void element has no storage (e.g. the payload of an all-void union), so there is no field
    if (inst.type && types_.translate(*inst.type)->isVoidTy()) {
        auto* dummy{llvm::UndefValue::get(types_.get_ptr_ty())};
        if (inst.result) { set_local(*inst.result, dummy); }
        return dummy;
    }

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
        if (!extract_indices.empty() && inst.operands[0].type &&
            (inst.operands[0].type->get_kind() == sema::type_kind::STRUCT ||
             inst.operands[0].type->get_kind() == sema::type_kind::CLOSURE)) {
            extract_indices.front() =
                types_.struct_field_index(base_ptr->getType(), extract_indices.front());
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
        case sema::type_kind::CLOSURE:
            source_elem_ty = types_.translate(base_type);
            indices.emplace_back(builder_.getInt32(0));
            for (const auto& operand : inst.operands | std::views::drop(1)) {
                auto* idx{lower_value(operand)};
                if (idx && idx->getType()->isIntegerTy(64)) {
                    idx = builder_.CreateIntCast(idx, types_.get_int32_ty(), false);
                }
                const bool is_field_index{indices.size() == 1 &&
                                          base_type.get_kind() != sema::type_kind::UNION};
                if (auto* ci{llvm::dyn_cast_or_null<llvm::ConstantInt>(idx)};
                    ci && is_field_index) {
                    const auto field{static_cast<u32>(ci->getZExtValue())};
                    idx = builder_.getInt32(types_.struct_field_index(source_elem_ty, field));
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
        case sema::type_kind::DYN:
            source_elem_ty = base_type.get_kind() == sema::type_kind::DYN
                                 ? static_cast<llvm::Type*>(types_.translate_dyn_fat_ptr())
                                 : types_.translate_slice_type();
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
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "AddressOf requires an operand");
    auto* val{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::emit_deref(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Deref requires an operand");
    auto* ptr{lower_value(inst.operands[0])};
    if (inst.result) { set_local(*inst.result, ptr); }
    return ptr;
}

auto llvm_lowering::emit_global_addr(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.callee_name, "global_addr requires the global's name");
    // Resolve by GIR name through `globals_` first to prevent ABI clash
    llvm::GlobalVariable* gv{nullptr};
    if (const auto it{globals_.find(*inst.callee_name)}; it != globals_.end()) {
        gv = llvm::dyn_cast<llvm::GlobalVariable>(it->second);
    }
    if (!gv) { gv = llvm_module_->getNamedGlobal(*inst.callee_name); }
    if (!gv) {
        for (const auto g : gir_module_->get_globals()) {
            if (g->name == *inst.callee_name) {
                gv = lower_global(*g);
                break;
            }
        }
    }
    if (inst.result) { set_local(*inst.result, gv); }
    return gv;
}

// `ptr + n` / `ptr - n` step by whole pointees, like C
auto llvm_lowering::emit_pointer_offset(const gir::instruction& inst) -> llvm::Value* {
    const auto& pointee{inst.operands[0].type->get_data().as<sema::types::pointer>().underlying};
    auto*       base{lower_value(inst.operands[0])};
    auto*       offset{lower_value(inst.operands[1])};
    ASSERT(base && offset, "Pointer arithmetic operands must lower to non-null LLVM values");

    auto*      index_ty{llvm_module_->getDataLayout().getIntPtrType(context_)};
    const auto offset_int{inst.operands[1].type ? sema::as_integer(*inst.operands[1].type)
                                                : stdx::option<sema::types::integer>{}};
    const bool is_signed_offset{!offset_int || offset_int->is_signed};
    offset = builder_.CreateIntCast(offset, index_ty, is_signed_offset, "ptr.offset");
    if (inst.kind == gir::instruction_kind::SUB) { offset = builder_.CreateNeg(offset); }
    return builder_.CreateGEP(types_.translate(pointee), base, offset, "ptr.step");
}

namespace {

// Whether `t` is `u0`, whose every value is 0 and whose LLVM form holds no bits
[[nodiscard]] auto is_zero_bit_int(stdx::option<const sema::type&> t) -> bool {
    return t && t->get_kind() == sema::type_kind::INT && sema::int_width(*t) == 0;
}

// A `u0` operand read as an integer: the constant 0
[[nodiscard]] auto as_int_operand(llvm::Value* v, stdx::option<const sema::type&> t)
    -> llvm::Value* {
    if (!is_zero_bit_int(t)) { return v; }
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(v->getContext()), 0);
}

} // namespace

auto llvm_lowering::emit_binary(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "Binary instruction requires at least 2 operands");
    if (is_zero_bit_int(inst.type)) {
        return llvm::ConstantAggregateZero::get(types_.translate(*inst.type));
    }
    const auto op0_ty{inst.operands[0].type};
    const auto op1_ty{inst.operands[1].type};
    const bool is_offset_op{inst.kind == gir::instruction_kind::ADD ||
                            inst.kind == gir::instruction_kind::SUB};
    if (is_offset_op && op0_ty && op0_ty->get_kind() == sema::type_kind::POINTER) {
        return emit_pointer_offset(inst);
    }
    const bool is_shift{inst.kind == gir::instruction_kind::SHL ||
                        inst.kind == gir::instruction_kind::SHR};
    const auto peer_ty{is_shift ? op0_ty : concrete_peer_type(op0_ty, op1_ty)};
    auto*      lhs{as_int_operand(lower_value(inst.operands[0], peer_ty), op0_ty)};
    auto* rhs{as_int_operand(lower_value(inst.operands[1], is_shift ? op1_ty : peer_ty), op1_ty)};
    ASSERT(lhs && rhs, "Binary operands must lower to non-null LLVM values");

    const bool is_flt{is_float_type(inst, lhs)};
    const bool is_sgn{is_signed_type(inst)};

    if (is_shift && !is_flt) {
        rhs = fit_shift_amount(lhs, rhs, op1_ty && sema::is_signed_integer(*op1_ty));
    } else if (!is_flt && lhs->getType() != rhs->getType()) {
        if (auto* lhs_int{llvm::dyn_cast<llvm::IntegerType>(lhs->getType())}) {
            if (auto* rhs_int{llvm::dyn_cast<llvm::IntegerType>(rhs->getType())}) {
                if (lhs_int->getBitWidth() < rhs_int->getBitWidth()) {
                    lhs = is_sgn ? builder_.CreateSExt(lhs, rhs_int, "arithext")
                                 : builder_.CreateZExt(lhs, rhs_int, "arithext");
                } else if (rhs_int->getBitWidth() < lhs_int->getBitWidth()) {
                    rhs = is_sgn ? builder_.CreateSExt(rhs, lhs_int, "arithext")
                                 : builder_.CreateZExt(rhs, lhs_int, "arithext");
                }
            }
        }
    }

    if (inst.is_saturating && !is_flt) { return emit_saturating_arith(inst, lhs, rhs, is_sgn); }
    if (inst.is_checked && !is_flt) {
        if (auto* checked{emit_checked_arith(inst, lhs, rhs, is_sgn)}) { return checked; }
    }

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
        if (is_flt) { return emit_frem(lhs, rhs); }
        return is_sgn ? builder_.CreateSRem(lhs, rhs, "modtmp")
                      : builder_.CreateURem(lhs, rhs, "modtmp");
    case gir::instruction_kind::AND: return builder_.CreateAnd(lhs, rhs, "andtmp");
    case gir::instruction_kind::OR:  return builder_.CreateOr(lhs, rhs, "ortmp");
    case gir::instruction_kind::XOR: return builder_.CreateXor(lhs, rhs, "xortmp");
    case gir::instruction_kind::SHL: {
        // Unchecked (`<<%`, or safety off): shifting every bit out leaves 0, not poison
        auto* int_ty{lhs->getType()};
        auto* in_range{builder_.CreateICmpULT(
            rhs, llvm::ConstantInt::get(rhs->getType(), int_ty->getScalarSizeInBits()))};
        auto* amount{
            builder_.CreateSelect(in_range, rhs, llvm::ConstantInt::get(rhs->getType(), 0))};
        return builder_.CreateSelect(
            in_range, builder_.CreateShl(lhs, amount, "shltmp"), llvm::ConstantInt::get(int_ty, 0));
    }
    case gir::instruction_kind::SHR:
        return is_sgn ? builder_.CreateAShr(lhs, rhs, "shrtmp")
                      : builder_.CreateLShr(lhs, rhs, "shrtmp");
    default: UNREACHABLE("Invalid instruction kind in emit_binary");
    }
}

auto llvm_lowering::emit_unary(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Unary instruction requires an operand");
    if (is_zero_bit_int(inst.type)) {
        return llvm::ConstantAggregateZero::get(types_.translate(*inst.type));
    }
    auto* val{lower_value(inst.operands[0])};
    ASSERT(val, "Unary operand must lower to a non-null LLVM value");

    const bool is_flt{is_float_type(inst, val)};

    if (inst.is_checked && !is_flt && inst.kind == gir::instruction_kind::NEG &&
        val->getType()->isIntOrIntVectorTy()) {
        auto* int_min{llvm::ConstantInt::get(
            val->getType(), llvm::APInt::getSignedMinValue(val->getType()->getScalarSizeInBits()))};
        emit_arith_guard(builder_.CreateICmpEQ(val, int_min), "signed negation overflow", inst);
    }

    switch (inst.kind) {
    case gir::instruction_kind::NEG:
        return is_flt ? builder_.CreateFNeg(val, "negtmp") : builder_.CreateNeg(val, "negtmp");
    case gir::instruction_kind::NOT:    return builder_.CreateNot(val, "nottmp");
    case gir::instruction_kind::BITNOT: return builder_.CreateNot(val, "bitnottmp");
    default:                            UNREACHABLE("Invalid instruction kind in emit_unary");
    }
}

auto llvm_lowering::emit_comparison(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.operands.size() >= 2, "Comparison requires 2 operands");
    const auto op0_ty{inst.operands[0].type};
    const auto op1_ty{inst.operands[1].type};
    const auto peer_ty{concrete_peer_type(op0_ty, op1_ty)};
    auto*      lhs{as_int_operand(lower_value(inst.operands[0], peer_ty), peer_ty)};
    auto*      rhs{as_int_operand(lower_value(inst.operands[1], peer_ty), peer_ty)};
    ASSERT(lhs && rhs, "Comparison operands must lower to non-null LLVM values");

    // A `^fn(...)` is null exactly when its code half is
    const auto callable_code{[&](llvm::Value* v) -> llvm::Value* {
        const bool fat{(op0_ty && sema::is_fat_callable(*op0_ty)) ||
                       (op1_ty && sema::is_fat_callable(*op1_ty))};
        if (!fat || !v->getType()->isStructTy()) { return v; }
        return builder_.CreateExtractValue(v, {1U}, "callable.code");
    }};
    lhs = callable_code(lhs);
    rhs = callable_code(rhs);

    const bool is_flt{is_float_type(inst, lhs)};
    const bool is_sgn{is_signed_type(inst)};

    if (!is_flt && lhs->getType() != rhs->getType()) {
        if (auto* lhs_int{llvm::dyn_cast<llvm::IntegerType>(lhs->getType())}) {
            if (auto* rhs_int{llvm::dyn_cast<llvm::IntegerType>(rhs->getType())}) {
                if (lhs_int->getBitWidth() < rhs_int->getBitWidth()) {
                    lhs = is_sgn ? builder_.CreateSExt(lhs, rhs_int, "cmpext")
                                 : builder_.CreateZExt(lhs, rhs_int, "cmpext");
                } else if (rhs_int->getBitWidth() < lhs_int->getBitWidth()) {
                    rhs = is_sgn ? builder_.CreateSExt(rhs, lhs_int, "cmpext")
                                 : builder_.CreateZExt(rhs, lhs_int, "cmpext");
                }
            }
        }
    }

    // Every narrower float format is exactly representable in a wider one
    if (is_flt && lhs->getType() != rhs->getType() && lhs->getType()->isFloatingPointTy() &&
        rhs->getType()->isFloatingPointTy()) {
        if (lhs->getType()->getPrimitiveSizeInBits() < rhs->getType()->getPrimitiveSizeInBits()) {
            lhs = builder_.CreateFPExt(lhs, rhs->getType(), "cmpext");
        } else {
            rhs = builder_.CreateFPExt(rhs, lhs->getType(), "cmpext");
        }
    }

    if (is_flt) {
        switch (inst.kind) {
        case gir::instruction_kind::EQ: return builder_.CreateFCmpOEQ(lhs, rhs, "cmptmp");
        case gir::instruction_kind::NE:
            return builder_.CreateFCmpUNE(lhs, rhs, "cmptmp"); // NaN != anything
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
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Cast instruction requires an operand");
    ASSERT(inst.type, "Cast instruction requires a target type");
    // The operand must lower as its own type, not the cast's target type
    auto* val{as_int_operand(lower_value(inst.operands[0]), inst.operands[0].type)};
    auto* target_ty{types_.translate(*inst.type)};
    ASSERT(val && target_ty, "Cast operand and target type must be valid");
    // Every value cast to `u0` is 0; whether it fit was checked before the cast
    if (is_zero_bit_int(inst.type)) { return llvm::ConstantAggregateZero::get(target_ty); }

    auto* src_ty{val->getType()};
    switch (inst.kind) {
    case gir::instruction_kind::INT_CAST: {
        const bool is_sgn{inst.operands[0].type &&
                                  inst.operands[0].type->get_kind() == sema::type_kind::COMPTIME_INT
                              ? !comptime_operand_is_nonnegative(inst.operands[0])
                              : is_signed_type(inst)};
        return builder_.CreateIntCast(val, target_ty, is_sgn);
    }
    case gir::instruction_kind::TRUNC_CAST: return builder_.CreateTrunc(val, target_ty, "trunc");
    case gir::instruction_kind::WIDEN_CAST: {
        const bool src_is_flt{src_ty->isFloatingPointTy()};
        const bool dst_is_flt{target_ty->isFloatingPointTy()};

        if (src_is_flt && dst_is_flt) { return builder_.CreateFPCast(val, target_ty, "fpcast"); }
        if (!src_is_flt && dst_is_flt) {
            return is_signed_type(inst) ? builder_.CreateSIToFP(val, target_ty, "sitofp")
                                        : builder_.CreateUIToFP(val, target_ty, "uitofp");
        }
        if (src_is_flt && !dst_is_flt) {
            const bool dst_is_sgn{sema::is_signed_integer(*inst.type)};
            if (inst.is_checked) { emit_float_to_int_guard(inst, val, target_ty, dst_is_sgn); }
            return dst_is_sgn ? builder_.CreateFPToSI(val, target_ty, "fptosi")
                              : builder_.CreateFPToUI(val, target_ty, "fptoui");
        }
        return builder_.CreateIntCast(val, target_ty, is_signed_type(inst));
    }
    case gir::instruction_kind::BIT_CAST: {
        if (!src_ty->isAggregateType() && !target_ty->isAggregateType()) {
            return builder_.CreateBitCast(val, target_ty, "bitcast");
        }
        // LLVM can't bitcast an array or struct (`u128` <-> `[2]u64`), so the bytes go through
        // a slot big enough for either view
        const auto& layout{llvm_module_->getDataLayout()};
        auto*       slot_ty{layout.getTypeAllocSize(src_ty) >= layout.getTypeAllocSize(target_ty)
                                ? src_ty
                                : target_ty};
        auto&       entry{builder_.GetInsertBlock()->getParent()->getEntryBlock()};
        llvm::IRBuilder<> entry_builder{&entry, entry.getFirstInsertionPt()};
        auto*             slot{entry_builder.CreateAlloca(slot_ty, nullptr, "bitcast.slot")};
        const auto        align{
            std::max(layout.getABITypeAlign(src_ty), layout.getABITypeAlign(target_ty))};
        slot->setAlignment(align);
        builder_.CreateAlignedStore(val, slot, align);
        return builder_.CreateAlignedLoad(target_ty, slot, align, "bitcast");
    }
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
    PROFILE_FUNCTION();
    ASSERT(!inst.operands.empty(), "Const instruction requires an operand");
    auto* val{lower_value(inst.operands[0], inst.type)};
    if (inst.result) { set_local(*inst.result, val); }
    return val;
}

auto llvm_lowering::resolve_named_function(std::string_view ghoti_name) -> llvm::Function* {
    PROFILE_FUNCTION();
    if (const auto it{globals_.find(ghoti_name)}; it != globals_.end()) {
        if (auto* fn{llvm::dyn_cast<llvm::Function>(it->second)}) { return fn; }
    }
    return llvm_module_->getFunction(ghoti_name);
}

auto llvm_lowering::resolve_named_constant(std::string_view name) -> llvm::Constant* {
    PROFILE_FUNCTION();
    if (auto* fn{resolve_named_function(name)}) { return fn; }
    if (const auto it{globals_.find(name)}; it != globals_.end()) {
        if (auto* c{llvm::dyn_cast<llvm::Constant>(it->second)}) { return c; }
    }
    if (auto* gv{llvm_module_->getNamedGlobal(name)}) { return gv; }
    if (auto* val{llvm_module_->getNamedValue(name)}) {
        if (auto* c{llvm::dyn_cast<llvm::Constant>(val)}) { return c; }
    }
    if (gir_module_) {
        for (const auto g : gir_module_->get_globals()) {
            if (g->name == name || (!g->link_name.empty() && g->link_name == name)) {
                return lower_global(*g);
            }
        }
        for (const auto f : gir_module_->get_functions()) {
            if (f->get_name() == name ||
                (!f->get_link_name().empty() && f->get_link_name() == name)) {
                return declare_function(*f);
            }
        }
    }
    return nullptr;
}

// `@call(modifier, ...)`: finds the call lowering just made after `before` and marks it
auto llvm_lowering::apply_call_modifier(ast::call_modifier modifier,
                                        llvm::BasicBlock&  block,
                                        llvm::Instruction* before) -> void {
    llvm::CallInst* call{nullptr};
    for (auto it{block.rbegin()}; it != block.rend() && &*it != before; ++it) {
        if (auto* candidate{llvm::dyn_cast<llvm::CallInst>(&*it)};
            candidate && !llvm::isa<llvm::IntrinsicInst>(candidate)) {
            call = candidate;
            break;
        }
    }
    if (!call) { return; }
    switch (modifier) {
    case ast::call_modifier::NEVER_TAIL: call->setTailCallKind(llvm::CallInst::TCK_NoTail); break;
    case ast::call_modifier::ALWAYS_TAIL:
        call->setTailCallKind(llvm::CallInst::TCK_MustTail);
        break;
    case ast::call_modifier::NEVER_INLINE:  call->addFnAttr(llvm::Attribute::NoInline); break;
    case ast::call_modifier::ALWAYS_INLINE: call->addFnAttr(llvm::Attribute::AlwaysInline); break;
    case ast::call_modifier::AUTO:
    case ast::call_modifier::COMPILE_TIME:  break;
    }
}

auto llvm_lowering::emit_call(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    if (inst.callee_name) {
        if (auto* callee_fn{resolve_named_function(*inst.callee_name)}) {
            std::vector<llvm::Value*> args;
            args.reserve(inst.operands.size());
            auto* callee_fn_ty{callee_fn->getFunctionType()};
            for (const auto& op : inst.operands) {
                if (op.type && sema::is_type_parameter_slot(*op.type)) { continue; }
                auto* arg_val{lower_value(op)};
                if (!arg_val || arg_val->getType()->isVoidTy()) { continue; }
                if (op.type && op.type->get_kind() == sema::type_kind::ARRAY &&
                    args.size() < callee_fn->getFunctionType()->getNumParams() &&
                    callee_fn->getFunctionType()
                        ->getParamType(static_cast<u32>(args.size()))
                        ->isStructTy()) {
                    if (const auto arr_data{op.type->get_data().as_opt<sema::types::array>()}) {
                        auto*        slice_ty{types_.translate_slice_type()};
                        llvm::Value* slice_val{llvm::UndefValue::get(slice_ty)};
                        llvm::Value* ptr_val{arg_val};
                        if (!ptr_val->getType()->isPointerTy()) {
                            auto* tmp{
                                builder_.CreateAlloca(arg_val->getType(), nullptr, "arr.tmp")};
                            builder_.CreateStore(arg_val, tmp);
                            ptr_val = tmp;
                        }
                        slice_val = builder_.CreateInsertValue(
                            slice_val, ptr_val, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                        slice_val = builder_.CreateInsertValue(
                            slice_val,
                            usize_const(static_cast<u64>(arr_data->len)),
                            {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
                        arg_val = slice_val;
                    }
                } else {
                    arg_val = load_aggregate_arg(op, arg_val);
                }
                if (args.size() >= callee_fn_ty->getNumParams() && callee_fn_ty->isVarArg()) {
                    arg_val = promote_c_variadic_arg(arg_val, op.type);
                }
                args.emplace_back(arg_val);
            }
            const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_ ||
                               inst.type->get_kind() == sema::type_kind::NORETURN};

            auto* call_inst{builder_.CreateCall(callee_fn, args, is_void ? "" : "calltmp")};
            call_inst->setCallingConv(callee_fn->getCallingConv());
            if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
            emit_testing_abort_check(*callee_fn);
            return call_inst;
        }
    }

    ASSERT(!inst.operands.empty(), "Indirect call requires callee operand");
    auto* callee_val{lower_value(inst.operands[0])};
    ASSERT(inst.operands[0].type, "Indirect callee must have function type");
    const auto ind_fn_data{sema::signature_of(*inst.operands[0].type)};
    ASSERT(ind_fn_data, "Indirect callee must have function type");
    // A folded `const f: fn(...) = g;` reaches here as `g` itself: call it directly
    const bool is_erased{ind_fn_data->erased && !llvm::isa<llvm::Function>(callee_val)};
    auto*      fn_ty{is_erased ? erased_code_type(*ind_fn_data)
                               : types_.translate_function_type(*ind_fn_data)};

    std::vector<llvm::Value*> args;
    args.reserve(inst.operands.size());
    if (is_erased) {
        auto* fat_ty{types_.translate(*inst.operands[0].type)};
        if (callee_val->getType()->isPointerTy()) {
            callee_val = builder_.CreateLoad(fat_ty, callee_val, "callable");
        }
        args.emplace_back(builder_.CreateExtractValue(callee_val, {0U}, "callable.ctx"));
        callee_val = builder_.CreateExtractValue(callee_val, {1U}, "callable.code");
    }
    for (const auto& operand : inst.operands | std::views::drop(1)) {
        // Match the direct-callee branch above
        if (operand.type && sema::is_type_parameter_slot(*operand.type)) { continue; }
        auto* arg_val{lower_value(operand)};
        if (!arg_val || arg_val->getType()->isVoidTy()) { continue; }
        arg_val = load_aggregate_arg(operand, arg_val);
        if (args.size() >= fn_ty->getNumParams() && fn_ty->isVarArg()) {
            arg_val = promote_c_variadic_arg(arg_val, operand.type);
        }
        args.emplace_back(arg_val);
    }

    const bool is_void{!inst.type || inst.type->get_kind() == sema::type_kind::VOID_ ||
                       inst.type->get_kind() == sema::type_kind::NORETURN};
    auto*      call_inst{builder_.CreateCall(fn_ty, callee_val, args, is_void ? "" : "calltmp")};
    call_inst->setAttributes(
        with_integer_extensions(call_inst->getAttributes(), *ind_fn_data, is_erased ? 1U : 0U));
    // An indirect call's target may have a non-default calling convention now that it's part of
    // `types::function`'s own identity
    call_inst->setCallingConv(
        is_erased
            ? llvm::CallingConv::C
            : to_llvm_callconv(ind_fn_data->conv, llvm::Triple{llvm_module_->getTargetTriple()}));
    if (inst.result && !is_void) { set_local(*inst.result, call_inst); }
    return call_inst;
}

// LLVM's generic `va_arg` expansion steps Win64's 8-byte slots by the type's own size, so walk
// them by hand the way clang does; larger or odd-sized values are passed by reference
auto llvm_lowering::emit_va_arg(llvm::Value* list, llvm::Type* ty) -> llvm::Value* {
    const llvm::Triple triple{llvm_module_->getTargetTriple()};
    if (!triple.isOSWindows() || triple.getArch() != llvm::Triple::x86_64) {
        return builder_.CreateVAArg(list, ty, "vaarg");
    }

    constexpr u64 win64_slot_size{8};
    const auto&   layout{llvm_module_->getDataLayout()};
    const auto    size{layout.getTypeAllocSize(ty).getFixedValue()};
    const bool    by_reference{size > win64_slot_size || !llvm::isPowerOf2_64(size)};

    auto* ptr_ty{types_.get_ptr_ty()};
    auto* slot{builder_.CreateLoad(ptr_ty, list, "va.slot")};
    auto* next{builder_.CreateConstInBoundsGEP1_64(
        builder_.getInt8Ty(), slot, win64_slot_size, "va.next")};
    builder_.CreateStore(next, list);
    auto* value_addr{by_reference ? builder_.CreateLoad(ptr_ty, slot, "va.indirect") : slot};
    return builder_.CreateLoad(ty, value_addr, "vaarg");
}

// C's default argument promotions: a variadic slot never carries a sub-`int` integer or a `float`
auto llvm_lowering::promote_c_variadic_arg(llvm::Value* arg_val, stdx::option<sema::type&> type)
    -> llvm::Value* {
    auto* arg_ty{arg_val->getType()};
    if (arg_ty->isIntegerTy() && arg_ty->getIntegerBitWidth() < 32) {
        bool is_signed{false};
        if (type) {
            if (const auto int_info{sema::as_integer(*type)}) { is_signed = int_info->is_signed; }
        }
        return builder_.CreateIntCast(arg_val, builder_.getInt32Ty(), is_signed, "vararg.promote");
    }
    if (arg_ty->isHalfTy() || arg_ty->isBFloatTy() || arg_ty->isFloatTy()) {
        return builder_.CreateFPExt(arg_val, builder_.getDoubleTy(), "vararg.promote");
    }
    return arg_val;
}

auto llvm_lowering::load_aggregate_arg(const gir::value& op, llvm::Value* arg_val) -> llvm::Value* {
    if (!op.type || !arg_val->getType()->isPointerTy()) { return arg_val; }
    const auto kind{op.type->get_kind()};
    const bool by_value_aggregate{
        kind == sema::type_kind::STRUCT || kind == sema::type_kind::UNION ||
        kind == sema::type_kind::SLICE || kind == sema::type_kind::CLOSURE ||
        sema::is_fat_callable(*op.type)};
    if (!by_value_aggregate) { return arg_val; }
    return builder_.CreateLoad(types_.translate(*op.type), arg_val, "struct_arg");
}

auto llvm_lowering::integer_extension(const sema::type& type)
    -> stdx::option<llvm::Attribute::AttrKind> {
    if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
        return integer_extension(en->underlying);
    }
    auto* llvm_ty{types_.translate(type)};
    if (!llvm_ty->isIntegerTy() || llvm_ty->getIntegerBitWidth() > 32) { return stdx::none; }

    const auto rule{narrow_int_rule_for(llvm::Triple{llvm_module_->getTargetTriple()})};
    if (llvm_ty->getIntegerBitWidth() == 32) {
        if (rule != narrow_int_rule::ALL_AND_RV64_WORD) { return stdx::none; }
        return llvm::Attribute::SExt;
    }
    if (rule == narrow_int_rule::NONE) { return stdx::none; }
    if (type.get_kind() == sema::type_kind::BOOL) { return llvm::Attribute::ZExt; }
    if (rule == narrow_int_rule::BOOL_ONLY) { return stdx::none; }
    return sema::is_signed_integer(type) ? llvm::Attribute::SExt : llvm::Attribute::ZExt;
}

auto llvm_lowering::with_integer_extensions(llvm::AttributeList          attrs,
                                            const sema::types::function& fn,
                                            u32 first_param) -> llvm::AttributeList {
    auto& context{llvm_module_->getContext()};
    if (const auto ext{integer_extension(fn.return_type)}) {
        attrs = attrs.addRetAttribute(context, *ext);
    }
    u32 idx{first_param};
    for (const auto param : fn.params) {
        if (types_.translate(*param)->isVoidTy()) { continue; }
        if (const auto ext{integer_extension(*param)}) {
            attrs = attrs.addParamAttribute(context, idx, *ext);
        }
        ++idx;
    }
    return attrs;
}

auto llvm_lowering::erased_code_type(const sema::types::function& fn) -> llvm::FunctionType* {
    auto*                    thin_ty{types_.translate_function_type(fn)};
    std::vector<llvm::Type*> params{types_.get_ptr_ty()};
    params.insert(params.end(), thin_ty->param_begin(), thin_ty->param_end());
    return llvm::FunctionType::get(thin_ty->getReturnType(), params, false);
}

auto llvm_lowering::get_or_create_fn_trampoline(const sema::types::function& fn)
    -> llvm::Function* {
    auto* thin_ty{types_.translate_function_type(fn)};
    if (const auto it{fn_trampolines_.find(thin_ty)}; it != fn_trampolines_.end()) {
        return it->second;
    }

    auto* trampoline{llvm::Function::Create(erased_code_type(fn),
                                            llvm::GlobalValue::InternalLinkage,
                                            private_symbol_name("fn_trampoline"),
                                            llvm_module_.get())};
    fn_trampolines_.emplace(thin_ty, trampoline);

    const llvm::IRBuilderBase::InsertPointGuard guard{builder_};
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", trampoline));
    std::vector<llvm::Value*> forwarded;
    for (auto& arg : trampoline->args() | std::views::drop(1)) { forwarded.emplace_back(&arg); }
    auto* call{builder_.CreateCall(thin_ty, trampoline->getArg(0), forwarded)};
    call->setCallingConv(to_llvm_callconv(fn.conv, llvm::Triple{llvm_module_->getTargetTriple()}));
    call->setTailCall();
    if (thin_ty->getReturnType()->isVoidTy()) {
        builder_.CreateRetVoid();
    } else {
        builder_.CreateRet(call);
    }
    return trampoline;
}

auto llvm_lowering::emit_make_callable(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.type && !inst.operands.empty(), "make_callable requires a type and a context");
    const auto fn{sema::fat_callable_fn(*inst.type)};
    ASSERT(fn, "make_callable must produce an erased fn");

    auto*        fat_ty{types_.translate(*inst.type)};
    auto*        ctx{lower_value(inst.operands[0])};
    llvm::Value* code{inst.callee_name ? resolve_named_function(*inst.callee_name)
                                       : get_or_create_fn_trampoline(*fn)};
    ASSERT(ctx && code, "make_callable operands must lower");

    llvm::Value* fat{llvm::PoisonValue::get(fat_ty)};
    fat = builder_.CreateInsertValue(fat, ctx, {0U});
    fat = builder_.CreateInsertValue(fat, code, {1U}, "callable");
    if (inst.result) { set_local(*inst.result, fat); }
    return fat;
}

namespace {

enum class reduce_op_t : u8 { // Mirrors builtin.gh.inc
    add  = 0,
    mul  = 1,
    band = 2,
    bor  = 3,
    bxor = 4,
    min  = 5,
    max  = 6,
};

} // namespace

auto llvm_lowering::scalarize(
    llvm::Type*                                                    result_ty,
    llvm::ArrayRef<llvm::Value*>                                   operands,
    llvm::function_ref<llvm::Value*(llvm::ArrayRef<llvm::Value*>)> scalar_op) -> llvm::Value* {
    const auto                lanes{llvm::cast<llvm::FixedVectorType>(result_ty)->getNumElements()};
    llvm::Value*              result{llvm::PoisonValue::get(result_ty)};
    std::vector<llvm::Value*> lane_args(operands.size());
    for (u32 lane{0}; lane < lanes; ++lane) {
        for (usize i{0}; i < operands.size(); ++i) {
            lane_args[i] = builder_.CreateExtractElement(operands[i], lane);
        }
        result = builder_.CreateInsertElement(result, scalar_op(lane_args), lane);
    }
    return result;
}

auto llvm_lowering::emit_repack(llvm::Value* from, llvm::Type* to) -> llvm::Value* {
    const bool   to_vector{to->isVectorTy()};
    const auto   lanes{to_vector ? llvm::cast<llvm::FixedVectorType>(to)->getNumElements()
                                 : static_cast<u32>(to->getArrayNumElements())};
    llvm::Value* result{llvm::PoisonValue::get(to)};
    for (u32 lane{0}; lane < lanes; ++lane) {
        result = to_vector ? builder_.CreateInsertElement(
                                 result, builder_.CreateExtractValue(from, {lane}), lane)
                           : builder_.CreateInsertValue(
                                 result, builder_.CreateExtractElement(from, lane), {lane});
    }
    return result;
}

auto llvm_lowering::emit_reduce(const gir::instruction& inst, llvm::Value* vec) -> llvm::Value* {
    const auto& lane{sema::lane_type_of(*inst.operands[0].type)};
    const bool  is_flt{sema::is_float(lane.get_kind())};
    const bool  is_sgn{sema::is_signed_integer(lane)};
    auto*       lane_ty{vec->getType()->getScalarType()};
    // A float sum or product folds its lanes in any order; `std.simd.reduce_ordered` keeps it
    const auto reassociated{[](llvm::Value* reduction) {
        llvm::cast<llvm::Instruction>(reduction)->setHasAllowReassoc(true);
        return reduction;
    }};
    switch (static_cast<reduce_op_t>(inst.lane_op.value_or(0))) {
    case reduce_op_t::add:
        if (!is_flt) { return builder_.CreateAddReduce(vec); }
        return reassociated(
            builder_.CreateFAddReduce(llvm::ConstantFP::getNegativeZero(lane_ty), vec));
    case reduce_op_t::mul:
        if (!is_flt) { return builder_.CreateMulReduce(vec); }
        return reassociated(builder_.CreateFMulReduce(llvm::ConstantFP::get(lane_ty, 1.0), vec));
    case reduce_op_t::band: return builder_.CreateAndReduce(vec);
    case reduce_op_t::bor:  return builder_.CreateOrReduce(vec);
    case reduce_op_t::bxor: return builder_.CreateXorReduce(vec);
    case reduce_op_t::min:
        if (is_flt) { return builder_.CreateFPMinReduce(vec); }
        return builder_.CreateIntMinReduce(vec, is_sgn);
    case reduce_op_t::max:
        if (is_flt) { return builder_.CreateFPMaxReduce(vec); }
        return builder_.CreateIntMaxReduce(vec, is_sgn);
    }
    UNREACHABLE("Unknown reduce op");
}

auto llvm_lowering::emit_vector_op(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.type, "A vector instruction has a result type");
    auto*      result_ty{types_.translate(*inst.type)};
    const auto lane_operand{[&](usize idx) -> llvm::Value* {
        return lower_value(inst.operands[idx], sema::lane_type_of(*inst.operands[0].type));
    }};
    switch (inst.kind) {
    case gir::instruction_kind::SPLAT: {
        auto* scalar{lower_value(inst.operands[0], sema::lane_type_of(*inst.type))};
        return builder_.CreateVectorSplat(
            llvm::cast<llvm::FixedVectorType>(result_ty)->getElementCount(), scalar, "splat");
    }
    case gir::instruction_kind::EXTRACT_LANE:
        return builder_.CreateExtractElement(
            lower_value(inst.operands[0]), lower_value(inst.operands[1]), "lane");
    case gir::instruction_kind::INSERT_LANE:
        return builder_.CreateInsertElement(
            lower_value(inst.operands[0]), lane_operand(2), lower_value(inst.operands[1]), "lanes");
    case gir::instruction_kind::REPACK:
        return emit_repack(lower_value(inst.operands[0]), result_ty);
    case gir::instruction_kind::SHUFFLE: {
        llvm::SmallVector<i32, 16> mask;
        for (const auto& entry : inst.operands | std::views::drop(2)) {
            const auto idx{entry.as_opt<i64>()};
            mask.emplace_back(idx ? static_cast<i32>(*idx) : llvm::PoisonMaskElem);
        }
        return builder_.CreateShuffleVector(
            lower_value(inst.operands[0]), lower_value(inst.operands[1]), mask, "shuffle");
    }
    case gir::instruction_kind::SELECT:
        return builder_.CreateSelect(lower_value(inst.operands[0]),
                                     lower_value(inst.operands[1], inst.type),
                                     lower_value(inst.operands[2], inst.type),
                                     "select");
    case gir::instruction_kind::REDUCE: return emit_reduce(inst, lower_value(inst.operands[0]));
    default:                            UNREACHABLE("Not a vector instruction");
    }
}

auto llvm_lowering::fixup_bit_count_result(llvm::Value* res, stdx::option<sema::type&> target)
    -> llvm::Value* {
    if (!target) { return res; }
    auto* target_ty{types_.translate(*target)};
    if (!target_ty || res->getType() == target_ty) { return res; }
    const auto res_bits{res->getType()->getIntegerBitWidth()};
    const auto target_bits{target_ty->getIntegerBitWidth()};
    if (target_bits > res_bits) { return builder_.CreateZExt(res, target_ty); }
    if (target_bits < res_bits) { return builder_.CreateTrunc(res, target_ty); }
    return res;
}

auto llvm_lowering::emit_frem(llvm::Value* lhs, llvm::Value* rhs) -> llvm::Value* {
    auto*       type{lhs->getType()};
    const auto  format{llvm_float_format(type)};
    const auto& triple{llvm_module_->getTargetTriple()};
    if (!needs_named_math_call(triple, format)) { return builder_.CreateFRem(lhs, rhs, "modtmp"); }
    // LLVM would call `fmodl`, which is `long double`'s and not this type's
    auto* fn_type{llvm::FunctionType::get(type, {type, type}, false)};
    auto  callee{llvm_module_->getOrInsertFunction(fmod_libcall_name(triple, format), fn_type)};
    auto* call{builder_.CreateCall(callee, {lhs, rhs}, "modtmp")};
    call->addFnAttr(llvm::Attribute::NoBuiltin);
    return call;
}

auto llvm_lowering::emit_math_call(math_function function, llvm::Value* operand) -> llvm::Value* {
    auto*      type{operand->getType()};
    const auto format{llvm_float_format(type)};
    // Exact operations: LLVM's intrinsics, and its folding of them, give the one right answer,
    // except where LLVM would call them by a `long double` name that isn't this type's
    const auto exact{[&] -> stdx::option<llvm::Intrinsic::ID> {
        // x87's `fsqrt` is exact, so only f80 `floor`/`ceil` need MSVC's named routines
        const bool native_sqrt{function == math_function::SQRT && format == float_format::X87};
        if (!native_sqrt && needs_named_math_call(llvm_module_->getTargetTriple(), format)) {
            return stdx::none;
        }
        switch (function) {
        case math_function::SQRT:  return llvm::Intrinsic::sqrt;
        case math_function::FLOOR: return llvm::Intrinsic::floor;
        case math_function::CEIL:  return llvm::Intrinsic::ceil;
        default:                   return stdx::none;
        }
    }()};
    if (exact) {
        auto* fn{llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), *exact, {type})};
        return builder_.CreateCall(fn, {operand});
    }

    // `f16` has no routine of its own; it goes through `f32` like LLVM's own libcalls do
    if (type->isHalfTy()) {
        auto* widened{builder_.CreateFPExt(operand, builder_.getFloatTy(), "math.ext")};
        return builder_.CreateFPTrunc(emit_math_call(function, widened), type, "math.trunc");
    }

    // A direct, `nobuiltin` call rather than `llvm.sin` and friends: LLVM folds those with the
    // host's libm, which would replace a correctly rounded constant with a host-dependent one
    const auto name{math_libcall_name(llvm_module_->getTargetTriple(), function, format)};
    auto*      fn_type{llvm::FunctionType::get(type, {type}, false)};
    auto       callee{llvm_module_->getOrInsertFunction(name, fn_type)};
    auto*      call{builder_.CreateCall(callee, {operand})};
    call->addFnAttr(llvm::Attribute::NoBuiltin);
    return call;
}

// `@llvmIntrinsic(name, ...)`: sema already matched the types against the intrinsic, so its
// overloads follow from the lowered operands
auto llvm_lowering::emit_llvm_intrinsic_call(const gir::instruction& inst) -> llvm::Value* {
    const auto id{llvm::Intrinsic::lookupIntrinsicID(*inst.callee_name)};
    ASSERT(id != llvm::Intrinsic::not_intrinsic, "@llvmIntrinsic names an intrinsic");

    std::vector<llvm::Value*> args;
    std::vector<llvm::Type*>  params;
    for (const auto& operand : inst.operands) {
        auto* arg{lower_value(operand)};
        args.emplace_back(arg);
        params.emplace_back(arg->getType());
    }
    auto* ret{inst.type ? types_.translate(*inst.type) : types_.get_void_ty()};
    auto* fn_type{llvm::FunctionType::get(ret, params, false)};

    llvm::SmallVector<llvm::Type*, 4> overloads;
    const bool matched{llvm::Intrinsic::isSignatureValid(id, fn_type, overloads)};
    ASSERT(matched, "@llvmIntrinsic's types were checked against the intrinsic");
    auto* callee{llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, overloads)};
    return builder_.CreateCall(callee, args);
}

auto llvm_lowering::emit_builtin_call(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    if (inst.callee_name && inst.callee_name->starts_with("llvm.")) {
        return emit_llvm_intrinsic_call(inst);
    }
    stdx::option<syntax::builtin_t> builtin_tok;
    if (inst.callee_name) {
        builtin_tok = syntax::builtin_from_name(*inst.callee_name);
        if (!builtin_tok && !inst.callee_name->starts_with('@')) {
            const auto at_name{fmt::format("@{}", *inst.callee_name)};
            builtin_tok = syntax::builtin_from_name(at_name);
        }
    }

    if (builtin_tok) {
        switch (*builtin_tok) {
        case syntax::builtin_t::PANIC:
        case syntax::builtin_t::TRAP:  {
            auto* trap_fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), llvm::Intrinsic::trap)};
            builder_.CreateCall(trap_fn, {});
            return nullptr;
        }
        case syntax::builtin_t::TEST_FUNCTIONS: return emit_test_table();
        case syntax::builtin_t::RETURN_ADDRESS: {
            // Overloaded on the code pointer it yields, which lives in the program address space
            const auto code_space{llvm_module_->getDataLayout().getProgramAddressSpace()};
            auto*      retaddr_fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(),
                                                        llvm::Intrinsic::returnaddress,
                                                        {builder_.getPtrTy(code_space)})};
            auto* call_val{builder_.CreateCall(retaddr_fn, {builder_.getInt32(0)})};
            auto* target_ty{inst.type ? types_.translate(*inst.type) : nullptr};
            if (!target_ty) { target_ty = builder_.getIntPtrTy(llvm_module_->getDataLayout()); }
            return builder_.CreatePtrToInt(call_val, target_ty, "return_address");
        }
        case syntax::builtin_t::EXPECT: {
            if (inst.operands.empty()) { return builder_.getInt1(true); }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return builder_.getInt1(true); }
            cond_val = lower_truthiness(cond_val);

            auto* cont_bb{branch_to_failure(cond_val, "expect")};
            auto* failed_flag{get_or_create_test_failed_flag()};
            builder_.CreateStore(builder_.getInt1(true), failed_flag);
            emit_context_handler_call(inst, "expect_handler", 4UZ, 1UZ);
            builder_.CreateBr(cont_bb);

            builder_.SetInsertPoint(cont_bb);
            if (inst.result) { set_local(*inst.result, cond_val); }
            return cond_val;
        }
        case syntax::builtin_t::REQUIRE: {
            if (inst.operands.empty()) { return nullptr; }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return nullptr; }
            cond_val = lower_truthiness(cond_val);

            auto* cont_bb{branch_to_failure(cond_val, "require")};
            auto* failed_flag{get_or_create_test_failed_flag()};
            builder_.CreateStore(builder_.getInt1(true), failed_flag);
            emit_context_handler_call(inst, "require_handler", 4UZ, 1UZ);
            if (lowering_fn_ && lowering_fn_->get_attributes().testing) {
                builder_.CreateStore(builder_.getInt1(true), get_or_create_test_abort_flag());
                emit_testing_abort_return();
            } else {
                builder_.CreateRet(builder_.getInt1(false));
            }

            builder_.SetInsertPoint(cont_bb);
            return nullptr;
        }
        case syntax::builtin_t::ASSERT:
        case syntax::builtin_t::VERIFY: {
            const bool is_verify{*builtin_tok == syntax::builtin_t::VERIFY};
            if (inst.operands.empty()) { return nullptr; }
            auto* cond_val{lower_value(inst.operands[0])};
            if (!cond_val) { return nullptr; }
            cond_val = lower_truthiness(cond_val);

            auto* cont_bb{branch_to_failure(cond_val, is_verify ? "verify" : "assert")};
            if (is_verify) {
                std::string_view msg{"verification failed"};
                if (inst.operands.size() > 4) {
                    if (const auto s{inst.operands[4].as_opt<std::string>()}) { msg = *s; }
                }
                emit_lowered_panic(msg, inst);
                builder_.CreateUnreachable();
            } else {
                emit_context_handler_call(inst, "assert_handler", 4UZ, 1UZ);
                builder_.CreateBr(cont_bb);
            }

            builder_.SetInsertPoint(cont_bb);
            return nullptr;
        }
        case syntax::builtin_t::SKIP: {
            builder_.CreateStore(builder_.getInt1(true), get_or_create_test_skipped_flag());
            emit_context_handler_call(inst, "skip_handler", 0UZ, 1UZ);
            // A test returns right after; a `@[testing]` helper unwinds to it here
            if (lowering_fn_ && lowering_fn_->get_attributes().testing) {
                builder_.CreateStore(builder_.getInt1(true), get_or_create_test_abort_flag());
                emit_testing_abort_return();
                builder_.SetInsertPoint(llvm::BasicBlock::Create(
                    context_, "skip.after", builder_.GetInsertBlock()->getParent()));
            }
            return nullptr;
        }
        case syntax::builtin_t::SRC: {
            ASSERT(inst.type, "@src must carry its SourceLocation result type");
            ASSERT(inst.operands.size() >= 3, "@src expects [file, line, column] operands");
            auto* struct_ty{llvm::cast<llvm::StructType>(types_.translate(*inst.type))};
            auto* slice_ty{types_.translate_slice_type()};

            llvm::Value* file_slice{llvm::UndefValue::get(slice_ty)};
            if (const auto path{inst.operands[0].as_opt<std::string>()}) {
                auto* gstr{builder_.CreateGlobalString(*path, "src.file")};
                file_slice = builder_.CreateInsertValue(
                    file_slice, gstr, {static_cast<u32>(gir::SLICE_PTR_FIELD_INDEX)});
                file_slice =
                    builder_.CreateInsertValue(file_slice,
                                               usize_const(path->size()),
                                               {static_cast<u32>(gir::SLICE_LEN_FIELD_INDEX)});
            }

            auto* line_v{lower_value(inst.operands[1])};
            auto* col_v{lower_value(inst.operands[2])};

            llvm::Value* agg{llvm::UndefValue::get(struct_ty)};
            agg = builder_.CreateInsertValue(agg, file_slice, {0U});
            if (line_v) { agg = builder_.CreateInsertValue(agg, line_v, {1U}); }
            if (col_v) { agg = builder_.CreateInsertValue(agg, col_v, {2U}); }
            return agg;
        }
        case syntax::builtin_t::TARGET_OS: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{std::string{triple.getOSName()}, inst.type});
        }
        case syntax::builtin_t::TARGET_ARCH: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{
                std::string{llvm::Triple::getArchTypeName(triple.getArch())}, inst.type});
        }
        case syntax::builtin_t::TARGET_TRIPLE: {
            const llvm::Triple triple{llvm_module_->getTargetTriple()};
            return lower_value(gir::value{triple.str(), inst.type});
        }

        // Memory operations
        case syntax::builtin_t::MEMCPY: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* src{lower_value(inst.operands[1])};
            auto* len{lower_value(inst.operands[2])};
            if (dest && src && len) {
                builder_.CreateMemCpy(dest, llvm::MaybeAlign(), src, llvm::MaybeAlign(), len);
            }
            return nullptr;
        }
        case syntax::builtin_t::MEMSET: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* dest{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            auto* len{lower_value(inst.operands[2])};
            if (dest && val && len) { builder_.CreateMemSet(dest, val, len, llvm::MaybeAlign()); }
            return nullptr;
        }
        case syntax::builtin_t::MEMMOVE: {
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
        case syntax::builtin_t::C_VA_START: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::vastart, {list->getType()})};
                builder_.CreateCall(fn, {list});
            }
            return nullptr;
        }
        case syntax::builtin_t::C_VA_COPY: {
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
        case syntax::builtin_t::C_VA_END: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::vaend, {list->getType()})};
                builder_.CreateCall(fn, {list});
            }
            return nullptr;
        }
        case syntax::builtin_t::C_VA_ARG: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* list{lower_value(inst.operands[0])}; list && inst.type) {
                if (auto* ty{types_.translate(*inst.type)}) { return emit_va_arg(list, ty); }
            }
            return nullptr;
        }

        // Masked and indexed memory operations, each lane aligned as its own type
        case syntax::builtin_t::MASKED_LOAD:
        case syntax::builtin_t::GATHER:      {
            VERIFY(inst.operands.size() == 3 && inst.type, "Arity verified during resolution");
            auto*      ptr{lower_value(inst.operands[0])};
            auto*      mask{lower_value(inst.operands[1])};
            auto*      passthru{lower_value(inst.operands[2], inst.type)};
            auto*      vec_ty{types_.translate(*inst.type)};
            const auto align{
                llvm_module_->getDataLayout().getABITypeAlign(vec_ty->getScalarType())};
            if (*builtin_tok == syntax::builtin_t::GATHER) {
                return builder_.CreateMaskedGather(vec_ty, ptr, align, mask, passthru, "gather");
            }
            return builder_.CreateMaskedLoad(vec_ty, ptr, align, mask, passthru, "masked.load");
        }
        case syntax::builtin_t::MASKED_STORE:
        case syntax::builtin_t::SCATTER:      {
            VERIFY(inst.operands.size() == 3, "Arity verified during resolution");
            const bool is_scatter{*builtin_tok == syntax::builtin_t::SCATTER};
            auto*      stored{lower_value(inst.operands[is_scatter ? 0 : 1])};
            auto*      ptr{lower_value(inst.operands[is_scatter ? 1 : 0])};
            auto*      mask{lower_value(inst.operands[2])};
            const auto align{
                llvm_module_->getDataLayout().getABITypeAlign(stored->getType()->getScalarType())};
            if (is_scatter) {
                builder_.CreateMaskedScatter(stored, ptr, align, mask);
            } else {
                builder_.CreateMaskedStore(stored, ptr, align, mask);
            }
            return nullptr;
        }

        case syntax::builtin_t::CLZ: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::ctlz, {val->getType()})};
                auto* res{builder_.CreateCall(fn, {val, builder_.getInt1(false)})};
                return fixup_bit_count_result(res, inst.type);
            }
            return nullptr;
        }
        case syntax::builtin_t::CTZ: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::cttz, {val->getType()})};
                auto* res{builder_.CreateCall(fn, {val, builder_.getInt1(false)})};
                return fixup_bit_count_result(res, inst.type);
            }
            return nullptr;
        }
        case syntax::builtin_t::POPCOUNT: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), llvm::Intrinsic::ctpop, {val->getType()})};
                auto* res{builder_.CreateCall(fn, {val})};
                return fixup_bit_count_result(res, inst.type);
            }
            return nullptr;
        }

        // Fused multiply add
        case syntax::builtin_t::MUL_ADD: {
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

        case syntax::builtin_t::SQRT:
        case syntax::builtin_t::SIN:
        case syntax::builtin_t::COS:
        case syntax::builtin_t::TAN:
        case syntax::builtin_t::EXP:
        case syntax::builtin_t::EXP2:
        case syntax::builtin_t::LOG:
        case syntax::builtin_t::LOG2:
        case syntax::builtin_t::LOG10:
        case syntax::builtin_t::FLOOR:
        case syntax::builtin_t::CEIL:  {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            auto* operand{lower_value(inst.operands[0])};
            if (!operand || !operand->getType()->isFloatingPointTy()) { return nullptr; }
            return emit_math_call(*gir::semantics::math_function_of(*builtin_tok), operand);
        }

        case syntax::builtin_t::ABS: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            if (auto* val{lower_value(inst.operands[0])}) {
                if (val->getType()->isFloatingPointTy()) {
                    auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                        llvm_module_.get(), llvm::Intrinsic::fabs, {val->getType()})};
                    return builder_.CreateCall(fn, {val});
                }
                if (val->getType()->isIntegerTy()) {
                    if (inst.is_checked) {
                        const auto width{val->getType()->getIntegerBitWidth()};
                        auto*      int_min{llvm::ConstantInt::get(
                            val->getType(), llvm::APInt::getSignedMinValue(width))};
                        emit_arith_guard(builder_.CreateICmpEQ(val, int_min),
                                         "signed absolute value overflow",
                                         inst);
                    }
                    auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                        llvm_module_.get(), llvm::Intrinsic::abs, {val->getType()})};
                    return builder_.CreateCall(fn, {val, builder_.getInt1(false)});
                }
            }
            return nullptr;
        }

        case syntax::builtin_t::FIELD_PARENT_PTR: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* field_ptr{lower_value(inst.operands[0])};
            if (!field_ptr || !inst.type) { return nullptr; }
            const auto ptr_data{inst.type->get_data().as_opt<sema::types::pointer>()};
            if (!ptr_data) { return nullptr; }
            auto* struct_ty{types_.translate(ptr_data->underlying)};
            if (!struct_ty || !struct_ty->isStructTy()) { return nullptr; }

            auto* idx_val{lower_value(inst.operands[1])};
            auto* idx_const{llvm::dyn_cast_or_null<llvm::ConstantInt>(idx_val)};
            if (!idx_const) { return nullptr; }
            const auto  field_idx{idx_const->getZExtValue()};
            const auto& layout{llvm_module_->getDataLayout()};
            const auto  elem_idx{types_.struct_field_index(struct_ty, static_cast<u32>(field_idx))};
            const auto  offset{layout.getStructLayout(llvm::cast<llvm::StructType>(struct_ty))
                                   ->getElementOffset(elem_idx)};

            auto* field_int{builder_.CreatePtrToInt(field_ptr, types_.get_usize_ty(), "fpp.i")};
            auto* parent_int{builder_.CreateSub(field_int, usize_const(offset), "fpp.base")};
            return builder_.CreateIntToPtr(parent_int, types_.get_ptr_ty(), "fpp.p");
        }

        case syntax::builtin_t::MIN:
        case syntax::builtin_t::MAX: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_max{*builtin_tok == syntax::builtin_t::MAX};
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            if (a->getType()->isFloatingPointTy()) {
                // IEEE minimumNumber/maximumNumber, spelled out so every float type lowers on
                // every target: a NaN loses, and equal zeros are ordered by sign (-0 < +0)
                auto* a_nan{builder_.CreateFCmpUNO(a, a)};
                auto* b_nan{builder_.CreateFCmpUNO(b, b)};
                auto* a_wins{is_max ? builder_.CreateFCmpOGT(a, b) : builder_.CreateFCmpOLT(a, b)};
                auto* equal{builder_.CreateFCmpOEQ(a, b)};
                auto* bits_ty{builder_.getIntNTy(
                    static_cast<u32>(a->getType()->getPrimitiveSizeInBits().getFixedValue()))};
                auto* a_negative{builder_.CreateICmpSLT(builder_.CreateBitCast(a, bits_ty),
                                                        llvm::ConstantInt::get(bits_ty, 0))};
                auto* zero_pick{builder_.CreateSelect(
                    is_max ? builder_.CreateNot(a_negative) : a_negative, a, b)};
                auto* ordered{
                    builder_.CreateSelect(a_wins, a, builder_.CreateSelect(equal, zero_pick, b))};
                return builder_.CreateSelect(a_nan, b, builder_.CreateSelect(b_nan, a, ordered));
            }
            auto  id{is_max ? (is_signed ? llvm::Intrinsic::smax : llvm::Intrinsic::umax)
                            : (is_signed ? llvm::Intrinsic::smin : llvm::Intrinsic::umin)};
            auto* fn{
                llvm::Intrinsic::getOrInsertDeclaration(llvm_module_.get(), id, {a->getType()})};
            return builder_.CreateCall(fn, {a, b});
        }

        case syntax::builtin_t::DIV_TRUNC:
        case syntax::builtin_t::REM:       {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            if (inst.is_checked) { emit_division_guards(inst, a, b, is_signed); }
            const bool is_rem{*builtin_tok == syntax::builtin_t::REM};
            if (is_rem) {
                return is_signed ? builder_.CreateSRem(a, b) : builder_.CreateURem(a, b);
            }
            return is_signed ? builder_.CreateSDiv(a, b) : builder_.CreateUDiv(a, b);
        }

        case syntax::builtin_t::DIV_EXACT: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            if (inst.is_checked) {
                emit_division_guards(inst, a, b, is_signed);
                auto* r{is_signed ? builder_.CreateSRem(a, b) : builder_.CreateURem(a, b)};
                emit_arith_guard(builder_.CreateICmpNE(r, llvm::ConstantInt::get(a->getType(), 0)),
                                 "exact division produced a remainder",
                                 inst);
            }
            // Unchecked, a remainder makes the quotient poison
            return is_signed ? builder_.CreateExactSDiv(a, b) : builder_.CreateExactUDiv(a, b);
        }

        case syntax::builtin_t::DIV_FLOOR:
        case syntax::builtin_t::MOD:       {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            if (!a || !b) { return nullptr; }
            const bool is_signed{inst.type && sema::is_signed_integer(*inst.type)};
            if (inst.is_checked) { emit_division_guards(inst, a, b, is_signed); }
            const bool is_mod{*builtin_tok == syntax::builtin_t::MOD};
            if (!is_signed) {
                return is_mod ? builder_.CreateURem(a, b) : builder_.CreateUDiv(a, b);
            }
            auto* zero{llvm::ConstantInt::get(a->getType(), 0)};
            if (is_mod) {
                // r = a %s b; ((r != 0) && ((r < 0) != (b < 0))) ? r + b : r
                auto* r{builder_.CreateSRem(a, b)};
                auto* r_nonzero{builder_.CreateICmpNE(r, zero)};
                auto* signs_differ{builder_.CreateICmpNE(builder_.CreateICmpSLT(r, zero),
                                                         builder_.CreateICmpSLT(b, zero))};
                auto* adjust{builder_.CreateAnd(r_nonzero, signs_differ)};
                return builder_.CreateSelect(adjust, builder_.CreateAdd(r, b), r);
            }
            // q = a /s b; ((a %s b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q
            auto* q{builder_.CreateSDiv(a, b)};
            auto* r{builder_.CreateSRem(a, b)};
            auto* r_nonzero{builder_.CreateICmpNE(r, zero)};
            auto* signs_differ{builder_.CreateICmpNE(builder_.CreateICmpSLT(a, zero),
                                                     builder_.CreateICmpSLT(b, zero))};
            auto* adjust{builder_.CreateAnd(r_nonzero, signs_differ)};
            return builder_.CreateSelect(
                adjust, builder_.CreateSub(q, llvm::ConstantInt::get(a->getType(), 1)), q);
        }

        case syntax::builtin_t::ADD_WITH_OVERFLOW:
        case syntax::builtin_t::SUB_WITH_OVERFLOW:
        case syntax::builtin_t::MUL_WITH_OVERFLOW:
        case syntax::builtin_t::SHL_WITH_OVERFLOW: {
            VERIFY(inst.operands.size() >= 3, "Arity mismatch not verified during resolution");
            auto* a{lower_value(inst.operands[0])};
            auto* b{lower_value(inst.operands[1])};
            auto* out_ptr{lower_value(inst.operands[2])};
            if (!a || !b || !out_ptr) { return nullptr; }
            const bool is_signed{inst.operands[0].type &&
                                 sema::is_signed_integer(*inst.operands[0].type)};

            llvm::Value* result{nullptr};
            llvm::Value* overflow{nullptr};
            if (*builtin_tok == syntax::builtin_t::SHL_WITH_OVERFLOW) {
                // The count keeps its own type. Shifting every bit out leaves 0, which overflowed
                // unless the value already was 0
                const bool count_signed{inst.operands[1].type &&
                                        sema::is_signed_integer(*inst.operands[1].type)};
                b = fit_shift_amount(a, b, count_signed);
                auto* zero{llvm::ConstantInt::get(a->getType(), 0)};
                auto* in_range{builder_.CreateICmpULT(
                    b, llvm::ConstantInt::get(a->getType(), a->getType()->getIntegerBitWidth()))};
                auto* amount{builder_.CreateSelect(in_range, b, zero)};
                auto* shifted{builder_.CreateShl(a, amount)};
                auto* back{is_signed ? builder_.CreateAShr(shifted, amount)
                                     : builder_.CreateLShr(shifted, amount)};
                result   = builder_.CreateSelect(in_range, shifted, zero);
                overflow = builder_.CreateSelect(
                    in_range, builder_.CreateICmpNE(back, a), builder_.CreateICmpNE(a, zero));
            } else {
                llvm::Intrinsic::ID id{};
                switch (*builtin_tok) {
                case syntax::builtin_t::ADD_WITH_OVERFLOW:
                    id = is_signed ? llvm::Intrinsic::sadd_with_overflow
                                   : llvm::Intrinsic::uadd_with_overflow;
                    break;
                case syntax::builtin_t::SUB_WITH_OVERFLOW:
                    id = is_signed ? llvm::Intrinsic::ssub_with_overflow
                                   : llvm::Intrinsic::usub_with_overflow;
                    break;
                default:
                    id = is_signed ? llvm::Intrinsic::smul_with_overflow
                                   : llvm::Intrinsic::umul_with_overflow;
                    break;
                }
                auto* fn{llvm::Intrinsic::getOrInsertDeclaration(
                    llvm_module_.get(), id, {a->getType()})};
                auto* pair{builder_.CreateCall(fn, {a, b})};
                result   = builder_.CreateExtractValue(pair, {0U});
                overflow = builder_.CreateExtractValue(pair, {1U});
            }
            builder_.CreateStore(result, out_ptr);
            return overflow;
        }

        case syntax::builtin_t::ATOMIC_LOAD: {
            VERIFY(!inst.operands.empty(), "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            if (!ptr || !inst.type) { return nullptr; }
            auto* type{types_.translate(*inst.type)};
            auto* load{builder_.CreateLoad(type, ptr)};
            // Naturally aligned like `atomicrmw`/`cmpxchg`, even where the ABI aligns less
            // (`u64` on i686 Linux), so it stays an instruction rather than a libcall
            load->setAlignment(natural_atomic_align(*llvm_module_, type));
            load->setAtomic(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return load;
        }
        case syntax::builtin_t::ATOMIC_STORE: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            if (!ptr || !val) { return nullptr; }
            auto* store{builder_.CreateStore(val, ptr)};
            store->setAlignment(natural_atomic_align(*llvm_module_, val->getType()));
            store->setAtomic(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return nullptr;
        }
        case syntax::builtin_t::ATOMIC_RMW: {
            VERIFY(inst.operands.size() >= 2, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* val{lower_value(inst.operands[1])};
            if (!ptr || !val) { return nullptr; }
            return builder_.CreateAtomicRMW(to_llvm_rmw_op(inst.atomic_op.value_or(0)),
                                            ptr,
                                            val,
                                            llvm::MaybeAlign(),
                                            to_llvm_ordering(inst.atomic_order.value_or(0)));
        }
        case syntax::builtin_t::CMPXCHG_WEAK:
        case syntax::builtin_t::CMPXCHG_STRONG: {
            VERIFY(inst.operands.size() >= 4, "Arity mismatch not verified during resolution");
            auto* ptr{lower_value(inst.operands[0])};
            auto* expected{lower_value(inst.operands[1])};
            auto* new_val{lower_value(inst.operands[2])};
            auto* out_ptr{lower_value(inst.operands[3])};
            if (!ptr || !expected || !new_val || !out_ptr) { return nullptr; }
            auto* cmpxchg{
                builder_.CreateAtomicCmpXchg(ptr,
                                             expected,
                                             new_val,
                                             llvm::MaybeAlign(),
                                             to_llvm_ordering(inst.atomic_order.value_or(0)),
                                             to_llvm_ordering(inst.atomic_fail_order.value_or(0)))};
            cmpxchg->setWeak(*builtin_tok == syntax::builtin_t::CMPXCHG_WEAK);
            auto* cx_result{builder_.CreateExtractValue(cmpxchg, {0U})};
            auto* success{builder_.CreateExtractValue(cmpxchg, {1U})};
            builder_.CreateStore(cx_result, out_ptr);
            return success;
        }
        case syntax::builtin_t::FENCE: {
            builder_.CreateFence(to_llvm_ordering(inst.atomic_order.value_or(0)));
            return nullptr;
        }

        default: break;
        }
    }
    return emit_call(inst);
}

auto llvm_lowering::emit_inline_asm(const gir::instruction& inst) -> llvm::Value* {
    PROFILE_FUNCTION();
    ASSERT(inst.asm_info, "INLINE_ASM instruction requires asm_info");
    const auto& info{*inst.asm_info};

    // Input operands become the asm call's arguments, in listed order.
    std::vector<llvm::Value*> args;
    std::vector<llvm::Type*>  param_tys;
    args.reserve(inst.operands.size());
    param_tys.reserve(inst.operands.size());
    for (const auto& op : inst.operands) {
        auto* val{lower_value(op)};
        ASSERT(val, "inline asm input operand lowered to null");
        args.emplace_back(val);
        param_tys.emplace_back(val->getType());
    }

    // One result per bound output operand, or a single result for the `_` result slot.
    std::vector<llvm::Type*> result_tys;
    if (info.has_result_slot) {
        ASSERT(inst.type, "inline asm result slot requires a result type");
        result_tys.emplace_back(types_.translate(*inst.type));
    } else {
        for (const auto& addr : info.output_addrs) {
            ASSERT(addr.type, "inline asm output operand requires a type");
            result_tys.emplace_back(types_.translate(*addr.type));
        }
    }

    llvm::Type* ret_ty{nullptr};
    if (result_tys.empty()) {
        ret_ty = types_.get_void_ty();
    } else if (result_tys.size() == 1) {
        ret_ty = result_tys.front();
    } else {
        ret_ty = llvm::StructType::get(context_, result_tys);
    }

    auto*      fn_ty{llvm::FunctionType::get(ret_ty, param_tys, false)};
    const bool has_side_effects{info.is_volatile || result_tys.empty()};
    const auto dialect{info.intel_dialect ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT};

    auto* inline_asm{llvm::InlineAsm::get(
        fn_ty, info.tmpl, info.constraints, has_side_effects, info.align_stack, dialect, false)};

    auto* call{builder_.CreateCall(fn_ty, inline_asm, args)};
    call->addFnAttr(llvm::Attribute::NoUnwind);
    // Assembler errors report this cookie back, so they can name the `asm` block's location
    if (inst.location) {
        const auto cookie{(static_cast<u64>(inst.location->line) << 32U) |
                          static_cast<u32>(inst.location->column)};
        call->setMetadata(
            "srcloc",
            llvm::MDNode::get(context_, llvm::ConstantAsMetadata::get(builder_.getInt64(cookie))));
    }
    if (info.is_noreturn) { call->setDoesNotReturn(); }

    // Store each bound output back through its target address.
    if (!info.has_result_slot && !info.output_addrs.empty()) {
        if (info.output_addrs.size() == 1) {
            builder_.CreateStore(call, lower_value(info.output_addrs.front()));
        } else {
            for (u32 i{0}; i < info.output_addrs.size(); ++i) {
                auto* field{builder_.CreateExtractValue(call, i)};
                builder_.CreateStore(field, lower_value(info.output_addrs[i]));
            }
        }
    }

    return info.has_result_slot ? call : nullptr;
}

auto llvm_lowering::emit_ret(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
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
    PROFILE_FUNCTION();
    ASSERT(inst.target_segment, "GOTO instruction requires a target segment");
    const auto it{segment_blocks_.find(*inst.target_segment)};
    ASSERT(it != segment_blocks_.end(), "Target segment block not found");
    builder_.CreateBr(it->second);
}

auto llvm_lowering::emit_cond_goto(const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();
    ASSERT(inst.true_segment && inst.false_segment,
           "COND_GOTO requires true and false target segments");
    ASSERT(!inst.operands.empty(), "COND_GOTO requires condition operand");

    auto*      cond_val{lower_value(inst.operands[0])};
    const auto true_it{segment_blocks_.find(*inst.true_segment)};
    const auto false_it{segment_blocks_.find(*inst.false_segment)};
    ASSERT(true_it != segment_blocks_.end() && false_it != segment_blocks_.end(),
           "COND_GOTO branch target blocks not found");
    auto*           branch{builder_.CreateCondBr(cond_val, true_it->second, false_it->second)};
    llvm::MDBuilder md{context_};
    if (inst.branch_weights) {
        const auto [on_true, on_false]{*inst.branch_weights};
        branch->setMetadata(llvm::LLVMContext::MD_prof, md.createBranchWeights(on_true, on_false));
    }
    if (inst.unpredictable) {
        branch->setMetadata(llvm::LLVMContext::MD_unpredictable, md.createUnpredictable());
    }
}

auto llvm_lowering::emit_unreachable() -> void { builder_.CreateUnreachable(); }

} // namespace ghoti::codegen
