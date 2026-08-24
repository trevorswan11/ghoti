#pragma once

#include <concepts>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/style.hh"

namespace ghoti {

// Should be zero indexed and only 1-indexed at print time
struct source_location {
    usize line{0};
    usize column{0};

    source_location() noexcept = default;
    source_location(usize line, usize column) noexcept : line{line}, column{column} {}

    auto operator==(const source_location&) const noexcept -> bool = default;
};

// A half-open [start, end) range; `end` is the position just past the last character
struct source_span {
    source_location start;
    source_location end;

    auto operator==(const source_span&) const noexcept -> bool = default;
};

template <typename T> struct source_info;

template <typename T>
concept Locateable = requires(T t) {
    { source_info<T>::get(t) } -> std::same_as<source_location>;
};

template <> struct source_info<std::pair<usize, usize>> {
    static auto get(const std::pair<usize, usize>& p) noexcept -> source_location {
        return {p.first, p.second};
    }
};

template <> struct source_info<source_location> {
    static auto get(const source_location& loc) -> source_location { return loc; }
};

namespace mod { struct module; } // namespace mod

// Mappings not to be changed, tied to LSP diagnostic emission
enum class diagnostic_level : u8 {
    ERROR   = 1,
    WARNING = 2,
};

namespace detail {

// Returns the level fit for diagnostic printing
[[nodiscard]] constexpr auto level_name(diagnostic_level level) noexcept -> std::string_view {
    switch (level) {
    case diagnostic_level::ERROR:   return "error";
    case diagnostic_level::WARNING: return "warning";
    default:                        return "";
    }
}

// Returns the level's style for diagnostic printing
[[nodiscard]] constexpr auto level_style(diagnostic_level level) noexcept {
    switch (level) {
    case diagnostic_level::ERROR:   return style::RED;
    case diagnostic_level::WARNING: return style::LIGHT_YELLOW;
    default:                        return style::BASE;
    }
}

// A decomposed diagnostic that contains all information for base formatting
struct formattable_diagnostic {
    const stdx::option<std::string>&      message;
    const stdx::option<source_location>&  location;
    std::string_view                      error_name;
    const stdx::option<diagnostic_level>& level;
};

auto format_diagnostic(std::ostream&                    os,
                       const formattable_diagnostic&    diag,
                       const stdx::option<std::string>& source_path,
                       stdx::option<bool>               in_terminal) -> std::ostream&;

} // namespace detail

struct diagnostic_snapshot {
    std::string                   message;
    stdx::option<source_location> location;
    std::string                   error_name;
    diagnostic_level              level;
};

template <stdx::ScopedEnum E> class diagnostic {
  public:
    explicit diagnostic(E err) noexcept : error_{err} {}
    diagnostic(E err, usize line, usize column) noexcept : loc_{{line, column}}, error_{err} {}
    diagnostic(stdx::option<std::string> msg, E err, usize line, usize column) noexcept
        : message_{std::move(msg)}, loc_{{line, column}}, error_{err} {}
    diagnostic(stdx::option<std::string> msg, E err, stdx::option<source_location> loc) noexcept
        : message_{std::move(msg)}, loc_{std::move(loc)}, error_{err} {}
    diagnostic(stdx::option<std::string> msg, E err) noexcept
        : message_{std::move(msg)}, error_{err} {}

    template <Locateable T>
    diagnostic(stdx::option<std::string> msg, E err, const T& t) noexcept
        : message_{std::move(msg)}, loc_{source_info<T>::get(t)}, error_{err} {}

    template <Locateable T> diagnostic(E err, T t) : loc_{source_info<T>::get(t)}, error_{err} {}

    // Moves the passed diagnostic into a new one with an error code
    diagnostic(diagnostic&& other, E err) noexcept
        : message_{std::move(other.message_)}, loc_{other.loc_}, error_{err} {}

    // Moves the passed diagnostic into a new one with a specified source location
    template <Locateable T>
    diagnostic(diagnostic&& other, const T& t) noexcept
        : message_{std::move(other.message_)}, loc_{source_info<T>::get(t)}, error_{other.error_} {}

    [[nodiscard]] auto to_string(const stdx::option<std::string>& source_path = stdx::none,
                                 stdx::option<bool> in_terminal = stdx::none) const -> std::string {
        std::ostringstream ss;
        detail::format_diagnostic(ss, to_formattable(), source_path, in_terminal);
        return ss.str();
    }

    auto operator==(const diagnostic& other) const noexcept -> bool {
        return message_ == other.message_ && loc_ == other.loc_ && error_ == other.error_ &&
               level_ == other.level_;
    }

    MAKE_GETTER(message, const stdx::option<std::string>&)
    MAKE_GETTER(error, E)
    [[nodiscard]] auto to_formattable() const noexcept -> detail::formattable_diagnostic {
        return {message_, loc_, magic_enum::enum_name(error_), level_};
    }

    // An owning copy, for callers that need this diagnostic's content to outlive its list
    [[nodiscard]] auto snapshot() const -> diagnostic_snapshot {
        return {message_.value_or(std::string{magic_enum::enum_name(error_)}),
               loc_,
               std::string{magic_enum::enum_name(error_)},
               level_.value_or(diagnostic_level::ERROR)};
    }

    // Diagnostics are always ERROR by default, see `unset_level`
    auto set_level(diagnostic_level level) noexcept -> void { level_.emplace(level); }
    auto unset_level() noexcept -> void { level_.reset(); }

  private:
    stdx::option<std::string>      message_;
    stdx::option<source_location>  loc_;
    E                              error_;
    stdx::option<diagnostic_level> level_{diagnostic_level::ERROR};
};

template <typename T> struct is_diagnostic : std::false_type {};
template <typename T> struct is_diagnostic<diagnostic<T>> : std::true_type {};

template <typename T>
concept Diagnostic = is_diagnostic<T>::value;

template <Diagnostic D> class diagnostic_list {
  public:
    using value_type = D;
    MAKE_ITERATOR(diagnostics, std::vector<D>, diagnostics_) // cppcheck-suppress syntaxError

  public:
    explicit diagnostic_list(stdx::option<bool> in_terminal = stdx::none) noexcept
        : in_terminal_{in_terminal} {}
    ~diagnostic_list() = default;

    MAKE_MOVE_ONLY(diagnostic_list)

    [[nodiscard]] auto operator[](usize idx) const noexcept -> const D& {
        return diagnostics_[idx];
    }

    auto push_back(const D& d) -> void { diagnostics_.push_back(d); }

    template <typename... Args> auto emplace_back(Args&&... args) -> void {
        diagnostics_.emplace_back(std::forward<Args>(args)...);
    }

    operator gsl::span<const D>() const { return diagnostics_; }

    // Creates a new list with the same terminal behavior
    [[nodiscard]] auto create_new() const -> diagnostic_list {
        return diagnostic_list{in_terminal_};
    }
    [[nodiscard]] auto get_terminal_status() const noexcept -> stdx::option<bool> {
        return in_terminal_;
    }

  private:
    diagnostics        diagnostics_;
    stdx::option<bool> in_terminal_;
};

} // namespace ghoti

template <> struct fmt::formatter<ghoti::source_location> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::source_location& loc, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}:{}", loc.line + 1, loc.column + 1);
    }
};

template <typename E> struct fmt::formatter<ghoti::diagnostic<E>> {
    static constexpr auto parse(format_parse_context& ctx) noexcept { return ctx.begin(); }

    static auto format(const ghoti::diagnostic<E>& d, format_context& ctx) {
        return fmt::format_to(ctx.out(), "{}", d.to_string());
    }
};
