#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <stdx/fixed/vector.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"

namespace llvm {

class DataLayout;
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
    std::string_view os;                    // linux macos ios windows freebsd ... freestanding
    std::string_view arch;                  // x86_64 aarch64 riscv64 wasm32 ...
    std::string_view abi;                   // gnu musl msvc android ... none
    std::string_view family;                // unix windows wasm other
    std::string_view endian;                // little big
    u32              ptr_bits;              // 16 / 32 / 64
    u32              c_long_bits{64};       // 32 on Windows and 32-bit targets
    bool             c_char_signed{true};   // `c_char` is unsigned on most non-x86 ABIs
    u32              c_longdouble_bits{64}; // 80 on x86, 128 on most 64-bit non-Apple ABIs

    [[nodiscard]] static auto resolve(const llvm::Triple& triple) noexcept -> target_facts;
    [[nodiscard]] static auto resolve(stdx::option<std::string_view> triple_str = stdx::none)
        -> target_facts;
};

// The scalar sizes and alignments that vary by target, read from LLVM's data layout so ghoti's
// struct layout matches both the LLVM types it lowers to and C (i386 SysV aligns `f64` and
// 64-bit ints to 4 inside structs, and its `f80` is 12 bytes)
struct type_layout {
    usize ptr_bytes;
    usize i64_align;
    usize f64_align;
    usize f80_size;
    usize f80_align;
    // The ABI alignment of a vector exactly `8 << k` bits wide, which a target may set apart from
    // its natural (size rounded up to a power of two) alignment
    std::array<usize, 11> vector_aligns;

    // A `bits`-wide vector's ABI alignment and allocated size, as LLVM lays it out
    [[nodiscard]] auto vector_align(u64 bits) const noexcept -> usize;
    [[nodiscard]] auto vector_size(u64 bits) const noexcept -> usize;

    [[nodiscard]] static auto resolve(const llvm::DataLayout& dl) -> type_layout;
    [[nodiscard]] static auto resolve(const llvm::Triple& triple) -> type_layout;
    [[nodiscard]] static auto resolve(stdx::option<std::string_view> triple_str = stdx::none)
        -> type_layout;
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
// Gives `module` the machine's data layout and triple. On RISC-V it also records the float ABI
// as the `target-abi` flag, which the C ABI lowering and the backend both read
auto configure_module_for_target(llvm::Module& module, const llvm::TargetMachine& machine) -> void;

// One architecture family's CPU features
struct cpu_feature_family {
    std::string_view         name; // The `builtin.<name>` namespace: `x86`, `aarch64`, `riscv`, ...
    std::vector<std::string> llvm_names;   // LLVM's spelling, in LLVM's (sorted) order
    std::vector<std::string> ghoti_names;  // Index-aligned with `llvm_names`
    std::vector<std::string> descriptions; // LLVM's summary of each, index-aligned too
};

// Every family whose LLVM backend is linked in, in a fixed order; built once and kept
[[nodiscard]] auto cpu_feature_families()
    -> stdx::fixed::vector<gsl::not_null<const cpu_feature_family*>, 10>;

// The family `triple`'s architecture belongs to, if ghoti knows one
[[nodiscard]] auto cpu_feature_family_of(const llvm::Triple& triple)
    -> stdx::option<const cpu_feature_family&>;

// The CPU code is generated for and the features it may use
struct target_cpu {
    std::string               model;
    std::vector<std::string>  enabled; // ghoti names, in family order
    stdx::option<std::string> family;
};

[[nodiscard]] auto resolve_target_cpu(const target_options& options)
    -> stdx::result<target_cpu, diagnostic>;

auto emit_object_file(llvm::Module&                module,
                      llvm::TargetMachine&         target_machine,
                      const std::filesystem::path& output_path) -> stdx::result<void, diagnostic>;

// Lowers `module` to native assembly for `target_machine` and returns it as text.
auto emit_asm_string(llvm::Module& module, llvm::TargetMachine& target_machine)
    -> stdx::result<std::string, diagnostic>;

} // namespace ghoti::codegen
