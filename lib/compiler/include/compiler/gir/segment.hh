#pragma once

#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"

namespace ghoti::gir {

class segment {
  public:
    constexpr explicit segment(usize id) noexcept : id_{id} {}
    ~segment() = default;
    MAKE_MOVE_ONLY(segment);

    MAKE_GETTER(id, usize);
    MAKE_DEDUCING_GETTER(instructions);

    auto append(instruction inst) -> instruction& {
        return instructions_.emplace_back(std::move(inst));
    }

    [[nodiscard]] auto has_terminator() const noexcept -> bool {
        return instructions_.empty() ? false : instructions_.back().is_terminator();
    }

    template <typename Self>
    [[nodiscard]] auto get_terminator_opt(this Self&& self) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, instruction&>> {
        if (!self.has_terminator()) { return stdx::none; }
        return self.instructions_.back();
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return instructions_.empty(); }
    [[nodiscard]] auto size() const noexcept -> usize { return instructions_.size(); }

  private:
    usize                    id_{0};
    std::vector<instruction> instructions_;
};

} // namespace ghoti::gir
