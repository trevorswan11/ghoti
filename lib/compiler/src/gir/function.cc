#include "compiler/gir/function.hh"

#include <string>
#include <utility>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto function::add_param(std::string name, sema::type& param_type) -> parameter& {
    const auto param_id{local_id::make_param(params_.size())};
    params_.emplace_back(parameter{.name = std::move(name), .type = param_type, .id = param_id});
    return params_.back();
}

auto function::add_segment() -> segment& {
    const auto new_id{segments_.size()};
    segments_.emplace_back(segment{new_id});
    return segments_.back();
}

} // namespace ghoti::gir
