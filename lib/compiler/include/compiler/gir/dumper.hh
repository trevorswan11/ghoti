#pragma once

#include <ostream>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"

namespace ghoti::gir {

class dumper {
  public:
    explicit dumper(std::ostream& out) noexcept : out_{out} {}

    auto dump(const module& mod) -> void;
    auto dump(const function& fn) -> void;
    auto dump(const segment& seg) -> void;
    auto dump(const instruction& inst) -> void;
    auto dump(const value& val) -> void;

  private:
    std::ostream& out_;
};

} // namespace ghoti::gir
