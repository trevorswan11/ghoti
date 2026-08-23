#pragma once

#include "ghoti/config.h" // IWYU pragma: export

#if GHOTI_WINDOWS

namespace ghoti::win32 {

// Enables UTF8 on creation, disables on destruction
//
// Can be safely created and destroyed multiple times on multiple threads
class rich_console {
  public:
    rich_console() noexcept;
    ~rich_console();
};

// Puts stdin/stdout in binary mode so CRLF translation can't corrupt byte-exact framing
auto set_binary_stdio() noexcept -> void;

} // namespace ghoti::win32

#endif
