#pragma once

#include <iostream>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"

namespace ghoti::cmd {

enum class command_kind : u8 {
    SHELL,
    LSP_SERVER,
    BUILD_EXE,
    BUILD_LIB,
    BUILD_OBJ,
    RUN,
    TEST,
    FORMAT,
};

class command {
  public:
    explicit command(std::ostream& error_stream = std::cerr) noexcept
        : error_stream_{error_stream} {}
    virtual ~command()                                                      = default;
    [[nodiscard]] virtual auto execute() -> stdx::result<void, clap::error> = 0;
    [[nodiscard]] virtual auto get_kind() const noexcept -> command_kind    = 0;

    template <typename T> [[nodiscard]] auto is() const noexcept -> bool {
        return get_kind() == T::KIND;
    }

    template <typename T> [[nodiscard]] auto as_opt() noexcept -> stdx::option<T&> {
        if (!is<T>()) { return stdx::none; }
        return *static_cast<T*>(this);
    }

  protected:
    std::ostream& error_stream_;
};

} // namespace ghoti::cmd
