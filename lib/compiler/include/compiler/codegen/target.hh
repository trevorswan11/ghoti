#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"

namespace llvm {

class Module;
class TargetMachine;
class Triple;
enum class CodeGenOptLevel;

} // namespace llvm

namespace ghoti::codegen {

enum class reloc_model : u8 {
    STATIC,
    PIC_,
    DYNAMIC_NO_PIC,
    ROPI,
    RWPI,
    ROPI_RWPI,
};

enum class code_model : u8 {
    TINY,
    SMALL,
    KERNEL,
    MEDIUM,
    LARGE,
};

enum class output_type : u8 {
    OBJECT,
    EXECUTABLE,
    STATIC_LIBRARY,
    DYNAMIC_LIBRARY,
};

struct target_options {
    stdx::option<std::string> triple_str{};
    std::string               cpu{"generic"};
    std::string               features{};
    opt_level                 level{opt_level::O0};
    stdx::option<reloc_model> reloc{reloc_model::PIC_};
    stdx::option<code_model>  code{};
};

// Initializes all LLVM target infos, targets, target MCs, and asm printers/parsers once.
auto               initialize_all_targets() noexcept -> void;
[[nodiscard]] auto resolve_target_triple(stdx::option<std::string_view> triple_str = stdx::none)
    -> llvm::Triple;

// Stable, normalized tokens describing the compilation target in @cfg preds
struct target_facts {
    std::string_view os;       // linux macos ios windows freebsd ... freestanding
    std::string_view arch;     // x86_64 aarch64 riscv64 wasm32 ...
    std::string_view abi;      // gnu musl msvc android ... none
    std::string_view family;   // unix windows wasm other
    std::string_view endian;   // little big
    u32              ptr_bits; // 16 / 32 / 64

    [[nodiscard]] static auto resolve(const llvm::Triple& triple) noexcept -> target_facts;
    [[nodiscard]] static auto resolve(stdx::option<std::string_view> triple_str = stdx::none)
        -> target_facts;
};

// Individual normalizers (also used to build `target_facts`).
[[nodiscard]] auto normalized_target_os(const llvm::Triple& triple) -> std::string_view;
[[nodiscard]] auto normalized_target_arch(const llvm::Triple& triple) -> std::string_view;
[[nodiscard]] auto normalized_target_abi(const llvm::Triple& triple) -> std::string_view;
[[nodiscard]] auto normalized_target_family(const llvm::Triple& triple) -> std::string_view;
[[nodiscard]] auto normalized_target_endian(const llvm::Triple& triple) -> std::string_view;
[[nodiscard]] auto normalized_target_ptr_bits(const llvm::Triple& triple) -> u32;

[[nodiscard]] auto get_default_output_extension(
    output_type type, stdx::option<std::string_view> triple_str = stdx::none) -> std::string_view;
[[nodiscard]] auto can_emit_freestanding_entry(const llvm::Triple& triple) -> bool;
[[nodiscard]] auto to_llvm_codegen_opt_level(opt_level level) noexcept -> llvm::CodeGenOptLevel;
[[nodiscard]] auto create_target_machine(const target_options& options)
    -> stdx::result<stdx::box<llvm::TargetMachine>, diagnostic>;

auto emit_object_file(llvm::Module&                module,
                      llvm::TargetMachine&         target_machine,
                      const std::filesystem::path& output_path) -> stdx::result<void, diagnostic>;

} // namespace ghoti::codegen
