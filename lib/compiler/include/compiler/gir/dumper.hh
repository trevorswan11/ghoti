#pragma once

#include <ostream>

#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/ip/pool.hh"

namespace ghoti::gir {

class dumper {
  public:
    explicit dumper(std::ostream&                 out,
                    stdx::option<const ip::pool&> constants = stdx::none) noexcept
        : out_{out}, constants_{constants} {}

    // Also prints what the plain dump leaves out: `.checked` arithmetic and each symbol's linkage
    auto set_show_flags(bool show) noexcept -> void { show_flags_ = show; }

    // Uses the module's own pool
    auto dump(const module_t& mod) -> void;
    // Uses the function's own pool
    auto dump(const function& fn) -> void;
    // Requires the dumper's `constants` to be set
    auto dump(const segment& seg) -> void;
    // Requires the dumper's `constants` to be set
    auto dump(const instruction& inst) -> void;
    // Requires the dumper's `constants` to be set
    auto dump(const value& val) -> void;

  private:
    [[nodiscard]] auto constants() const -> const ip::pool&;

  private:
    std::ostream&                 out_;
    stdx::option<const ip::pool&> constants_;
    bool                          show_flags_{false};
};

} // namespace ghoti::gir
