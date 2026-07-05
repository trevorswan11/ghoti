#include "compiler/gir/function.hh"

#include <string>
#include <utility>

#include <stdx/arena.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto function::add_param(stdx::arena<GIR_ARENA_BLOCK_SIZE>& arena,
                         std::string                        name,
                         sema::type&                        param_type) -> parameter& {
    const auto param_id{local_id::make_param(params_.size())};
    return *params_.emplace_back(arena.make<parameter>(std::move(name), param_type, param_id));
}

auto function::add_segment(stdx::arena<GIR_ARENA_BLOCK_SIZE>& arena) -> segment& {
    const auto new_id{segments_.size()};
    return *segments_.emplace_back(arena.make<segment>(new_id));
}

} // namespace ghoti::gir
