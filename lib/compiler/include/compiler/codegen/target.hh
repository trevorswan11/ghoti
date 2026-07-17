#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <llvm/Support/CodeGen.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>

#include "compiler/codegen/error.hh"
#include "compiler/codegen/opt_level.hh"

namespace llvm {

class Module;
class TargetMachine;
class Triple;

} // namespace llvm

namespace ghoti::codegen {

struct target_options {
    stdx::option<std::string>            triple_str{};
    std::string                          cpu{"generic"};
    std::string                          features{};
    opt_level                            level{opt_level::O0};
    stdx::option<llvm::Reloc::Model>     reloc_model{llvm::Reloc::PIC_};
    stdx::option<llvm::CodeModel::Model> code_model{};
};

// Initializes all LLVM target infos, targets, target MCs, and asm printers/parsers once.
auto               initialize_all_targets() noexcept -> void;
[[nodiscard]] auto resolve_target_triple(stdx::option<std::string_view> triple_str = stdx::none)
    -> llvm::Triple;

[[nodiscard]] constexpr auto to_llvm_codegen_opt_level(opt_level level) noexcept
    -> llvm::CodeGenOptLevel {
    switch (level) {
    case opt_level::O0: return llvm::CodeGenOptLevel::None;
    case opt_level::O1: return llvm::CodeGenOptLevel::Less;
    case opt_level::O2: return llvm::CodeGenOptLevel::Default;
    case opt_level::O3: return llvm::CodeGenOptLevel::Aggressive;
    case opt_level::Os: return llvm::CodeGenOptLevel::Default;
    case opt_level::Oz: return llvm::CodeGenOptLevel::Default;
    default:            return llvm::CodeGenOptLevel::None;
    }
}

[[nodiscard]] auto create_target_machine(const target_options& options)
    -> stdx::result<stdx::box<llvm::TargetMachine>, diagnostic>;

auto emit_object_file(llvm::Module&                module,
                      llvm::TargetMachine&         target_machine,
                      const std::filesystem::path& output_path) -> stdx::result<void, diagnostic>;

} // namespace ghoti::codegen
