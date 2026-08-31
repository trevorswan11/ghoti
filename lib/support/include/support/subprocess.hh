#pragma once

#include <chrono>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <stdx/iterator.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

namespace ghoti {

class mock_argv {
  public:
    MAKE_ITERATOR(args_t, std::vector<char*>, pointers_)

  public:
    template <stdx::StringLike... Strings> explicit mock_argv(Strings&&... args) {
        strings_.reserve(sizeof...(Strings));
        (..., strings_.emplace_back(args));

        pointers_.reserve(strings_.size() + 1);
        for (auto& s : strings_) { pointers_.emplace_back(s.data()); }
        pointers_.emplace_back(nullptr);
    }

    explicit mock_argv(std::vector<std::string> args) : strings_{std::move(args)} {
        pointers_.reserve(strings_.size() + 1);
        for (auto& s : strings_) { pointers_.emplace_back(s.data()); }
        pointers_.emplace_back(nullptr);
    }

    [[nodiscard]] auto argv() const noexcept -> char** { return pointers_.data(); }
    template <stdx::NumericIntegral I = i32> [[nodiscard]] auto argc() const noexcept -> I {
        return static_cast<I>(strings_.size());
    }

    [[nodiscard]] auto operator[](usize idx) const noexcept -> char* { return pointers_[idx]; }

  private:
    std::vector<std::string> strings_;
    mutable args_t           pointers_;
};

// Returns the path of the current process' running exe
[[nodiscard]] auto self_exe_path() -> std::filesystem::path;

// Quotes one argument per Win32's argv-splitting rules (see CommandLineToArgvW)
[[nodiscard]] auto quote_arg_windows(std::string_view arg) -> std::string;

// Exit code returned by spawn_child when the child is killed for exceeding its timeout
constexpr u32 spawn_child_timeout_exit_code{124};

[[nodiscard]] auto spawn_child(const mock_argv&          args,
                               std::chrono::milliseconds timeout = std::chrono::seconds{30})
    -> stdx::option<u32>;

// zig-out/tests/*.exe -> zig-out/bin/ghoti(.exe)
[[nodiscard]] auto ghoti_binary_path() -> std::filesystem::path;

// A child process with its stdin/stdout piped through streams and stderr inherited
class piped_process {
  public:
    explicit piped_process(const mock_argv& args);
    ~piped_process();
    MAKE_MOVE_ONLY(piped_process)

    [[nodiscard]] auto is_running() const noexcept -> bool;

    // Write end of the child's stdin
    [[nodiscard]] auto stdin_stream() noexcept -> std::ostream&;
    // Read end of the child's stdout
    [[nodiscard]] auto stdout_stream() noexcept -> std::istream&;
    // Read end of the child's stderr
    [[nodiscard]] auto stderr_stream() noexcept -> std::istream&;

    // Closes the child's stdin, then blocks until it exits, returning its exit code
    [[nodiscard]] auto close_stdin_and_wait() -> stdx::option<u32>;

  private:
    struct impl;

  private:
    stdx::box<impl> impl_;
};

} // namespace ghoti
