#include "compiler/codegen/runtime_libcalls.hh"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <llvm/IR/RuntimeLibcalls.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/hash.hh>
#include <stdx/types.hh>

#include "support/float128.hh"
#include "support/float_math.hh"

namespace ghoti::codegen {

namespace {

using name_set = ankerl::unordered_dense::
    set<std::string, stdx::string_transparent_hash, stdx::string_transparent_eq>;
// Node-based so the references handed out stay valid as other triples are added
using triple_map = std::map<std::string, name_set, std::less<>>;

[[nodiscard]] auto libcall_names(const llvm::Triple& triple) -> const name_set& {
    static std::mutex mutex;
    static triple_map by_triple;

    const std::scoped_lock lock{mutex};
    if (const auto it{by_triple.find(triple.str())}; it != by_triple.end()) { return it->second; }

    const llvm::RTLIB::RuntimeLibcallsInfo info{triple};
    name_set                               names;
    for (const auto impl : info.getLibcallImpls()) {
        if (impl == llvm::RTLIB::Unsupported) { continue; }
        if (const auto* name{llvm::RTLIB::RuntimeLibcallsInfo::getLibcallImplName(impl)}) {
            names.emplace(name);
        }
    }
    return by_triple.emplace(triple.str(), std::move(names)).first->second;
}

} // namespace

auto is_runtime_libcall(const llvm::Triple& triple, std::string_view symbol) -> bool {
    return libcall_names(triple).contains(symbol);
}

auto is_compiler_rt_symbol(const llvm::Triple& triple, std::string_view symbol) -> bool {
    // libc provides these on every hosted target, and ghoti synthesizes them when freestanding
    constexpr std::array<std::string_view, 5> libc_memory{
        "memcpy", "memmove", "memset", "memcmp", "bcmp"};
    if (std::ranges::contains(libc_memory, symbol)) { return false; }
    if (triple.isWindowsMSVCEnvironment() && symbol == "_fltused") { return true; }
    return is_runtime_libcall(triple, symbol);
}

auto math_libcall_name(const llvm::Triple& triple, math_function function, float_format format)
    -> std::string {
    using llvm::RTLIB::Libcall;
    // f32, f64, f80, f128
    const auto  variants{[&]() -> std::array<Libcall, 4> {
        namespace rt = llvm::RTLIB;
        switch (function) {
        case math_function::SIN:  return {rt::SIN_F32, rt::SIN_F64, rt::SIN_F80, rt::SIN_F128};
        case math_function::COS:  return {rt::COS_F32, rt::COS_F64, rt::COS_F80, rt::COS_F128};
        case math_function::TAN:  return {rt::TAN_F32, rt::TAN_F64, rt::TAN_F80, rt::TAN_F128};
        case math_function::EXP:  return {rt::EXP_F32, rt::EXP_F64, rt::EXP_F80, rt::EXP_F128};
        case math_function::EXP2: return {rt::EXP2_F32, rt::EXP2_F64, rt::EXP2_F80, rt::EXP2_F128};
        case math_function::LOG:  return {rt::LOG_F32, rt::LOG_F64, rt::LOG_F80, rt::LOG_F128};
        case math_function::LOG2: return {rt::LOG2_F32, rt::LOG2_F64, rt::LOG2_F80, rt::LOG2_F128};
        case math_function::LOG10:
            return {rt::LOG10_F32, rt::LOG10_F64, rt::LOG10_F80, rt::LOG10_F128};
        case math_function::SQRT: return {rt::SQRT_F32, rt::SQRT_F64, rt::SQRT_F80, rt::SQRT_F128};
        case math_function::FLOOR:
            return {rt::FLOOR_F32, rt::FLOOR_F64, rt::FLOOR_F80, rt::FLOOR_F128};
        case math_function::CEIL: return {rt::CEIL_F32, rt::CEIL_F64, rt::CEIL_F80, rt::CEIL_F128};
        }
        std::unreachable();
    }()};
    const usize index{format == float_format::SINGLE   ? 0UZ
                      : format == float_format::DOUBLE ? 1UZ
                      : format == float_format::X87    ? 2UZ
                                                       : 3UZ};
    // LLVM names the f128 routines `sinl` where `long double` is binary128 and even where it is
    // not (MSVC, macOS), which would bind to the platform's own `double`/x87 `sinl`. f128 always
    // takes the unambiguous C23 name, and so does f80 where `long double` is `double`.
    if (format == float_format::QUAD) {
        return fmt::format("{}f128", math_function_name(function));
    }
    if (format == float_format::X87 && triple.isWindowsMSVCEnvironment()) {
        return fmt::format("{}f64x", math_function_name(function));
    }
    const llvm::RTLIB::RuntimeLibcallsInfo info{triple};
    if (const auto* name{info.getLibcallName(variants[index])}) { return name; }

    constexpr std::array suffixes{"f", "", "l", "f128"};
    return fmt::format("{}{}", math_function_name(function), suffixes[index]);
}

auto strip_global_prefix(const llvm::Triple& triple, std::string_view symbol) -> std::string_view {
    const bool prefixed{triple.isOSBinFormatMachO() ||
                        (triple.isOSBinFormatCOFF() && triple.getArch() == llvm::Triple::x86)};
    if (prefixed && symbol.starts_with('_')) { symbol.remove_prefix(1); }
    return symbol;
}

} // namespace ghoti::codegen
