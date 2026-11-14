#pragma once

#include <ostream>

#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"

namespace ghoti::gir {

// Dumping a module or a function reads its constants from that one's pool. A segment,
// instruction, or value dumped alone needs `constants` given
class dumper {
  public:
    explicit dumper(std::ostream&                 out,
                    stdx::option<const ip::pool&> constants = stdx::none) noexcept
        : out_{out}, constants_{constants} {}

    auto dump(const module_t& mod) -> void;
    auto dump(const function& fn) -> void;
    auto dump(const segment& seg) -> void;
    auto dump(const instruction& inst) -> void;
    auto dump(const value& val) -> void;

  private:
    [[nodiscard]] auto constants() const -> const ip::pool&;

  private:
    std::ostream&                 out_;
    stdx::option<const ip::pool&> constants_;
};

} // namespace ghoti::gir
