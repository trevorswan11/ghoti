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

auto reachable_segments(const function& fn) -> std::vector<bool> {
    const auto&       segments{fn.get_segments()};
    std::vector<bool> reachable(segments.size(), false);
    if (segments.empty()) { return reachable; }

    std::vector<segment_id> worklist{segments[0]->get_id()};
    reachable[0] = true;
    while (!worklist.empty()) {
        const auto current{fn.get_segment_opt(worklist.back())};
        worklist.pop_back();
        if (!current) { continue; }
        for (const auto* inst : (*current)->get_instructions()) {
            for (const auto successor :
                 {inst->target_segment, inst->true_segment, inst->false_segment}) {
                if (!successor) { continue; }
                const auto target{std::to_underlying(*successor)};
                if (target < segments.size() && !reachable[target]) {
                    reachable[target] = true;
                    worklist.push_back(*successor);
                }
            }
        }
    }
    return reachable;
}

} // namespace ghoti::gir
