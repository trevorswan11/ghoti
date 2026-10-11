#pragma once

#include <array>
#include <string_view>

// The members of each enum-typed `@cfg` atom. `builtin.gh.inc` declares the matching `builtin`
// enums; a test keeps the two in step.
namespace ghoti::sema::cfg_atoms {

inline constexpr std::array<std::string_view, 14> OS_MEMBERS{"linux",
                                                             "macos",
                                                             "ios",
                                                             "windows",
                                                             "freebsd",
                                                             "openbsd",
                                                             "netbsd",
                                                             "dragonfly",
                                                             "solaris",
                                                             "haiku",
                                                             "wasi",
                                                             "emscripten",
                                                             "uefi",
                                                             "freestanding"};
inline constexpr std::array<std::string_view, 17> ARCH_MEMBERS{"x86_64",
                                                               "x86",
                                                               "aarch64",
                                                               "arm",
                                                               "thumb",
                                                               "riscv64",
                                                               "riscv32",
                                                               "wasm32",
                                                               "wasm64",
                                                               "powerpc64",
                                                               "powerpc",
                                                               "mips64",
                                                               "mips",
                                                               "s390x",
                                                               "loongarch64",
                                                               "sparc64",
                                                               "sparc"};
inline constexpr std::array<std::string_view, 14> ABI_MEMBERS{"gnu",
                                                              "gnueabi",
                                                              "gnueabihf",
                                                              "gnux32",
                                                              "gnuabi64",
                                                              "musl",
                                                              "musleabi",
                                                              "musleabihf",
                                                              "msvc",
                                                              "android",
                                                              "eabi",
                                                              "eabihf",
                                                              "itanium",
                                                              "none"};
inline constexpr std::array<std::string_view, 4> FAMILY_MEMBERS{"unix", "windows", "wasm", "other"};
inline constexpr std::array<std::string_view, 2> ENDIAN_MEMBERS{"little", "big"};
inline constexpr std::array<std::string_view, 4> OPTIMIZE_MEMBERS{
    "debug", "release_safe", "release_fast", "release_small"};

} // namespace ghoti::sema::cfg_atoms
