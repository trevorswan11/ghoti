#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace llvm { class TargetMachine; } // namespace llvm

namespace ghoti::codegen {

enum class opt_level : u8 {
    O0, // Default: no optimization
    O1, // Basic optimization
    O2, // Release default: balanced optimization
    O3, // Aggressive optimization
    Os, // Size optimization
    Oz, // Aggressive size optimization
};

[[nodiscard]] auto parse_opt_level(std::string_view str) noexcept -> stdx::option<opt_level>;

struct optimizer_options {
    llvm::TargetMachine* target_machine{nullptr};
    opt_level            level{opt_level::O0};
    bool                 verify_each{true};
    bool                 debug_logging{false};
    bool                 time_passes{false};
};

} // namespace ghoti::codegen
