#include "support/subprocess.hh"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <ios>
#include <istream>
#include <ostream>
#include <streambuf>
#include <string>

#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <sys/signal.h>

#include "ghoti/config.h"

#if GHOTI_WINDOWS
#    include <iterator>
#    include <ranges>
#    include <string_view>

#    define WIN32_LEAN_AND_MEAN
#    include <fileapi.h>
#    include <handleapi.h>
#    include <libloaderapi.h>
#    include <minwinbase.h>
#    include <minwindef.h>
#    include <namedpipeapi.h>
#    include <processenv.h>
#    include <processthreadsapi.h>
#    include <synchapi.h>
#    include <windows.h>
#elif GHOTI_APPLE
#    include <mach-o/dyld.h>
#endif

#if !GHOTI_WINDOWS
#    include <csignal>
#    include <signal.h>
#    include <stdlib.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace ghoti {

auto quote_arg_windows(std::string_view arg) -> std::string {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string{arg};
    }

    std::string out{"\""};
    usize       backslashes{0};
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            backslashes = 0;
            out += '"';
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

auto self_exe_path() -> std::filesystem::path {
    using namespace stdx::size_literals;
    std::array<char, 1_KiB> buffer{};

#if GHOTI_WINDOWS
    static_assert(MAX_PATH < buffer.size(), "max path exceeds buffer size");
    ::GetModuleFileNameA(nullptr, buffer.data(), buffer.size());
#elif GHOTI_APPLE
    u32 size{sizeof(buffer)};
    ::_NSGetExecutablePath(buffer.data(), &size);
#else
    const auto len{::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1)};
    if (len != -1) { buffer[static_cast<usize>(len)] = '\0'; }
#endif

    return buffer.data();
}

auto spawn_child(const mock_argv& args) -> stdx::option<u32> {
#if GHOTI_WINDOWS
    auto cmd_line{quote_arg_windows(args[0])};
    for (const auto& arg : args | std::views::drop(1)) {
        if (!arg) { break; }
        fmt::format_to(std::back_inserter(cmd_line), " {}", quote_arg_windows(arg));
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

namespace {

#if GHOTI_WINDOWS

// Adapts a Win32 pipe HANDLE to std::streambuf; unbuffered writes, small read-ahead buffer
class handle_streambuf final : public std::streambuf {
  public:
    explicit handle_streambuf(::HANDLE handle) noexcept : handle_{handle} {}

  protected:
    auto underflow() -> int_type override {
        ::DWORD read{0};
        if (!::ReadFile(
                handle_, buffer_.data(), static_cast<::DWORD>(buffer_.size()), &read, nullptr) ||
            read == 0) {
            return traits_type::eof();
        }
        setg(buffer_.data(), buffer_.data(), buffer_.data() + read);
        return traits_type::to_int_type(buffer_.front());
    }

    auto xsputn(const char_type* s, std::streamsize n) -> std::streamsize override {
        ::DWORD written{0};
        if (!::WriteFile(handle_, s, static_cast<::DWORD>(n), &written, nullptr)) { return 0; }
        return static_cast<std::streamsize>(written);
    }

    auto overflow(int_type ch) -> int_type override {
        if (ch == traits_type::eof()) { return traits_type::not_eof(ch); }
        const char_type c{traits_type::to_char_type(ch)};
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }

  private:
    ::HANDLE                handle_;
    std::array<char, 4'096> buffer_{};
};

#else

// Adapts a POSIX pipe fd to std::streambuf; unbuffered writes, small read-ahead buffer
class handle_streambuf final : public std::streambuf {
  public:
    explicit handle_streambuf(i32 fd) noexcept : fd_{fd} {}

  protected:
    auto underflow() -> int_type override {
        const auto read{::read(fd_, buffer_.data(), buffer_.size())};
        if (read <= 0) { return traits_type::eof(); }
        setg(buffer_.data(), buffer_.data(), buffer_.data() + read);
        return traits_type::to_int_type(buffer_.front());
    }

    auto xsputn(const char_type* s, std::streamsize n) -> std::streamsize override {
        const auto written{::write(fd_, s, static_cast<usize>(n))};
        return std::max(std::streamsize{0}, written);
    }

    auto overflow(int_type ch) -> int_type override {
        if (ch == traits_type::eof()) { return traits_type::not_eof(ch); }
        const char_type c{traits_type::to_char_type(ch)};
        return xsputn(&c, 1) == 1 ? ch : traits_type::eof();
    }

  private:
    i32                                     fd_;
    std::array<char, stdx::sizes::kib(4UZ)> buffer_{};
};

#endif

} // namespace

struct piped_process::impl {
#if GHOTI_WINDOWS
    ::HANDLE process;
    ::HANDLE thread;
    ::HANDLE stdin_write;
    ::HANDLE stdout_read;
    ::HANDLE stderr_read;
#else
    i32 pid;
    i32 stdin_write;
    i32 stdout_read;
    i32 stderr_read;
#endif
    handle_streambuf stdin_buf;
    handle_streambuf stdout_buf;
    handle_streambuf stderr_buf;
    std::ostream     stdin_stream;
    std::istream     stdout_stream;
    std::istream     stderr_stream;
    // Whether the process handle / pid has already been waited on
    bool reaped;

#if GHOTI_WINDOWS
    impl(::HANDLE proc, ::HANDLE thr, ::HANDLE in_w, ::HANDLE out_r, ::HANDLE err_r)
        : process{proc}, thread{thr}, stdin_write{in_w}, stdout_read{out_r}, stderr_read{err_r},
          stdin_buf{stdin_write}, stdout_buf{stdout_read}, stderr_buf{stderr_read},
          stdin_stream{&stdin_buf}, stdout_stream{&stdout_buf}, stderr_stream{&stderr_buf},
          reaped{false} {}
#else
    impl(i32 child_pid, i32 in_w, i32 out_r, i32 err_r)
        : pid{child_pid}, stdin_write{in_w}, stdout_read{out_r}, stderr_read{err_r},
          stdin_buf{stdin_write}, stdout_buf{stdout_read}, stderr_buf{stderr_read},
          stdin_stream{&stdin_buf}, stdout_stream{&stdout_buf}, stderr_stream{&stderr_buf},
          reaped{false} {}
#endif
};

piped_process::~piped_process() {
    if (!impl_) { return; }
#if GHOTI_WINDOWS
    if (!impl_->reaped) {
        ::TerminateProcess(impl_->process, 1);
        ::CloseHandle(impl_->process);
        ::CloseHandle(impl_->thread);
        ::CloseHandle(impl_->stdin_write);
    }
    ::CloseHandle(impl_->stdout_read);
    ::CloseHandle(impl_->stderr_read);
#else
    if (!impl_->reaped) {
        ::kill(impl_->pid, SIGKILL);
        i32 status;
        ::waitpid(impl_->pid, &status, 0);
        ::close(impl_->stdin_write);
    }
    ::close(impl_->stdout_read);
    ::close(impl_->stderr_read);
#endif
}

auto piped_process::is_running() const noexcept -> bool { return impl_ && !impl_->reaped; }
auto piped_process::stdin_stream() noexcept -> std::ostream& { return impl_->stdin_stream; }
auto piped_process::stdout_stream() noexcept -> std::istream& { return impl_->stdout_stream; }
auto piped_process::stderr_stream() noexcept -> std::istream& { return impl_->stderr_stream; }

#if GHOTI_WINDOWS

piped_process::piped_process(const mock_argv& args) {
    ::SECURITY_ATTRIBUTES sa;
    ::ZeroMemory(&sa, sizeof(sa));
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    ::HANDLE stdin_read{nullptr}, stdin_write{nullptr};
    ::HANDLE stdout_read{nullptr}, stdout_write{nullptr};
    ::HANDLE stderr_read{nullptr}, stderr_write{nullptr};
    ::CreatePipe(&stdin_read, &stdin_write, &sa, 0);
    ::CreatePipe(&stdout_read, &stdout_write, &sa, 0);
    ::CreatePipe(&stderr_read, &stderr_write, &sa, 0);

    // Only the child's ends should be inherited; keep our own ends out of the child's handle table
    ::SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    auto cmd_line{quote_arg_windows(args[0])};
    for (const auto& arg : args | std::views::drop(1)) {
        if (!arg) { break; }
        fmt::format_to(std::back_inserter(cmd_line), " {}", quote_arg_windows(arg));
    }

    ::STARTUPINFOA si;
    ::ZeroMemory(&si, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError  = stderr_write;

    ::PROCESS_INFORMATION pi;
    ::ZeroMemory(&pi, sizeof(pi));

    // A just-linked exe can be locked while antivirus scans it on first execution
    bool ok{false};
    for (i32 attempt{0}; attempt < 60 && !ok; ++attempt) {
        if (attempt > 0) { ::Sleep(200); }
        ok = ::CreateProcessA(
            nullptr, cmd_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    }

    // The child now has its own copies of these; the parent's copies would block EOF detection
    ::CloseHandle(stdin_read);
    ::CloseHandle(stdout_write);
    ::CloseHandle(stderr_write);

    if (!ok) {
        ::CloseHandle(stdin_write);
        ::CloseHandle(stdout_read);
        ::CloseHandle(stderr_read);
        return;
    }

    impl_ = stdx::make_box<impl>(pi.hProcess, pi.hThread, stdin_write, stdout_read, stderr_read);
}

auto piped_process::close_stdin_and_wait() -> stdx::option<u32> {
    if (!impl_ || impl_->reaped) { return stdx::none; }

    ::CloseHandle(impl_->stdin_write);
    ::WaitForSingleObject(impl_->process, INFINITE);
    ::DWORD exit_code{0};
    ::GetExitCodeProcess(impl_->process, &exit_code);
    ::CloseHandle(impl_->process);
    ::CloseHandle(impl_->thread);
    impl_->reaped = true;
    return static_cast<u32>(exit_code);
}

#else

piped_process::piped_process(const mock_argv& args) {
    i32 in_pipe[2], out_pipe[2], err_pipe[2];
    if (::pipe(in_pipe) < 0 || ::pipe(out_pipe) < 0 || ::pipe(err_pipe) < 0) { return; }

    const auto pid{::fork()};
    if (pid < 0) { return; }

    if (pid == 0) {
        // Child
        ::dup2(in_pipe[0], STDIN_FILENO);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(in_pipe[0]);
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        ::close(err_pipe[0]);
        ::close(err_pipe[1]);
        ::execvp(args[0], args.argv());
        std::_Exit(127); // execvp failed
    }

    // Parent
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    impl_ = stdx::make_box<impl>(static_cast<i32>(pid), in_pipe[1], out_pipe[0], err_pipe[0]);
}

auto piped_process::close_stdin_and_wait() -> stdx::option<u32> {
    if (!impl_ || impl_->reaped) { return stdx::none; }

    ::close(impl_->stdin_write);
    i32 status{0};
    if (::waitpid(impl_->pid, &status, 0) < 0) { return stdx::none; }
    impl_->reaped = true;

    if (WIFEXITED(status)) { return static_cast<u32>(WEXITSTATUS(status)); }
    if (WIFSIGNALED(status)) { return static_cast<u32>(128 + WTERMSIG(status)); }
    return stdx::none;
}

#endif

} // namespace ghoti
