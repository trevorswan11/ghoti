#include "support/subprocess.hh"

#include <cstdlib>
#include <string>

#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "ghoti/config.h"

#if GHOTI_WINDOWS
#    include <iterator>
#    include <ranges>

#    define WIN32_LEAN_AND_MEAN
#    include <handleapi.h>
#    include <libloaderapi.h>
#    include <minwinbase.h>
#    include <minwindef.h>
#    include <processenv.h>
#    include <processthreadsapi.h>
#    include <synchapi.h>
#    include <windows.h>
#elif GHOTI_APPLE
#    include <mach-o/dyld.h>
#endif

#if !GHOTI_WINDOWS
#    include <stdlib.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace ghoti {

auto spawn_child(const mock_argv& args) -> stdx::option<u32> {
#if GHOTI_WINDOWS
    auto cmd_line{fmt::format(R"("{}")", args[0])};
    for (const auto& arg : args | std::views::drop(1)) {
        if (!arg) { break; }
        fmt::format_to(std::back_inserter(cmd_line), " {}", arg);
    }

    // I don't think you understand how much I hate working with the windows API
    ::STARTUPINFOA si;
    ::ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);

    ::PROCESS_INFORMATION pi;
    ::ZeroMemory(&pi, sizeof(pi));

    if (!::CreateProcessA(
            nullptr, cmd_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        return stdx::none;
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    ::DWORD exit_code;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
    return static_cast<u32>(exit_code);
#else
    const auto pid{::fork()};
    if (pid < 0) { return stdx::none; }

    if (pid == 0) {
        // Child
        ::execvp(args[0], args.argv());
        std::_Exit(127); // execvp failed
    } else {
        // Parent
        i32 status;
        if (::waitpid(pid, &status, 0) < 0) { return stdx::none; }

        if (WIFEXITED(status)) { return static_cast<u32>(WEXITSTATUS(status)); }
        if (WIFSIGNALED(status)) { return static_cast<u32>(128 + WTERMSIG(status)); }
    }
    return stdx::none;
#endif
}

} // namespace ghoti
