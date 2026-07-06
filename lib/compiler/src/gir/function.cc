#include "compiler/gir/function.hh"

#include <string>
#include <utility>

#include <stdx/arena.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto function::add_param(std::string name, sema::type& param_type) -> parameter& {
    const auto param_id{local_id::make_param(params_.size())};
    return *params_.emplace_back(arena_.make<parameter>(std::move(name), param_type, param_id));
}

auto function::add_segment() -> segment& {
    const auto new_id{static_cast<segment_id>(segments_.size())};
    return *segments_.emplace_back(arena_.make<segment>(arena_, new_id));
}

} // namespace ghoti::gir
