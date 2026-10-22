#pragma once

#include <string>
#include <string_view>

#include "support/float128.hh"
#include "support/float_math.hh"

namespace llvm { class Triple; } // namespace llvm

namespace ghoti::codegen {

// True when LLVM may emit a call to `symbol` on its own for the target. `symbol` is the
// source-level name.
[[nodiscard]] auto is_runtime_libcall(const llvm::Triple& triple, std::string_view symbol) -> bool;

// True when an undefined `symbol` in an object is something compiler_rt exists to provide
[[nodiscard]] auto is_compiler_rt_symbol(const llvm::Triple& triple, std::string_view symbol)
    -> bool;

// The routine a runtime `@sin`-style call on a `format` float reaches: LLVM's libcall name for
// f32/f64 (and f80 where `long double` is x87), compiler_rt's `__sinx` for f80 without x87, and
// the C23 name (`sinf128`, `sinf64x`) otherwise
[[nodiscard]] auto math_libcall_name(const llvm::Triple& triple,
                                     math_function       function,
                                     float_format        format) -> std::string;

// True when LLVM's own name for `format`'s `floor`/`ceil`/`sqrt`/`fmod` would be ambiguous: f128
// everywhere (`floorl` is `long double`'s), and f80 on MSVC (where `long double` is `double`)
[[nodiscard]] auto needs_named_math_call(const llvm::Triple& triple, float_format format) -> bool;

// The `fmod` routine a runtime float `%` reaches, named like `math_libcall_name`
[[nodiscard]] auto fmod_libcall_name(const llvm::Triple& triple, float_format format)
    -> std::string;

// Drops the global symbol prefix an object file adds to source-level names
[[nodiscard]] auto strip_global_prefix(const llvm::Triple& triple, std::string_view symbol)
    -> std::string_view;

} // namespace ghoti::codegen
