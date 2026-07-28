#include "driver/platform/win32.hh"

// "Abandon hope, all ye who enter here"
#if GHOTI_WINDOWS
#    define WIN32_LEAN_AND_MEAN
#    define NOMINMAX
#    include <atomic>
#    include <windows.h>

#    include <consoleapi.h>
#    include <consoleapi2.h>
#    include <handleapi.h>
#    include <minwindef.h>
#    include <processenv.h>
#    include <winnls.h>

#    include <stdx/profiler.hh>
#    include <stdx/types.hh>

namespace ghoti::win32 {

namespace {

std::atomic<i32> ref_count{0};
UINT             original_code_page{0};
DWORD            original_stdout_mode{0};
DWORD            original_stderr_mode{0};

} // namespace

rich_console::rich_console() noexcept {
    PROFILE_FUNCTION();
    if (ref_count.fetch_add(1) > 0) { return; }
    original_code_page = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);

    if (auto* stdout_h{GetStdHandle(STD_OUTPUT_HANDLE)}; stdout_h != INVALID_HANDLE_VALUE) {
        GetConsoleMode(stdout_h, &original_stdout_mode);
        SetConsoleMode(stdout_h, original_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    if (auto* stderr_h{GetStdHandle(STD_ERROR_HANDLE)}; stderr_h != INVALID_HANDLE_VALUE) {
        GetConsoleMode(stderr_h, &original_stderr_mode);
        SetConsoleMode(stderr_h, original_stderr_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
}

rich_console::~rich_console() {
    PROFILE_FUNCTION();
    if (ref_count.fetch_sub(1) != 1) { return; }
    SetConsoleOutputCP(original_code_page);

    if (auto* stdout_h{GetStdHandle(STD_OUTPUT_HANDLE)}; stdout_h != INVALID_HANDLE_VALUE) {
        SetConsoleMode(stdout_h, original_stdout_mode);
    }

    if (auto* stderr_h{GetStdHandle(STD_ERROR_HANDLE)}; stderr_h != INVALID_HANDLE_VALUE) {
        SetConsoleMode(stderr_h, original_stderr_mode);
    }
}

} // namespace ghoti::win32

#endif
