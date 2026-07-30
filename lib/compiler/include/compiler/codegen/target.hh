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

[[nodiscard]] auto to_llvm_codegen_opt_level(opt_level level) noexcept -> llvm::CodeGenOptLevel;

[[nodiscard]] auto create_target_machine(const target_options& options)
    -> stdx::result<stdx::box<llvm::TargetMachine>, diagnostic>;

auto emit_object_file(llvm::Module&                module,
                      llvm::TargetMachine&         target_machine,
                      const std::filesystem::path& output_path) -> stdx::result<void, diagnostic>;

} // namespace ghoti::codegen
