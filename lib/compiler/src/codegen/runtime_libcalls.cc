#include "compiler/codegen/runtime_libcalls.hh"

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <llvm/IR/RuntimeLibcalls.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/hash.hh>

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

auto strip_global_prefix(const llvm::Triple& triple, std::string_view symbol) -> std::string_view {
    const bool prefixed{triple.isOSBinFormatMachO() ||
                        (triple.isOSBinFormatCOFF() && triple.getArch() == llvm::Triple::x86)};
    if (prefixed && symbol.starts_with('_')) { symbol.remove_prefix(1); }
    return symbol;
}

} // namespace ghoti::codegen
