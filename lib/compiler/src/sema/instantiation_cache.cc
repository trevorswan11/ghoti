#include "compiler/sema/instantiation_cache.hh"

#include <algorithm>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/const_eval.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

auto body_typing_snapshot::diff_into(context&                            ctx,
                                     mod::module&                        m,
                                     body_type_diff&                     out,
                                     stdx::option<const body_write_log&> write_log) const -> void {
    std::vector<std::pair<stdx::option<type&>*, type*>> folded;
    std::vector<usize>                                  folded_node_idxs, folded_explicit_idxs;
    {
        gir::const_eval folder{ctx, m};
        const auto      fold{[&](auto& slots, std::vector<usize>& folded_idxs) {
            for (usize i{0}; i < slots.size(); ++i) {
                auto& slot{slots[i]};
                if (!slot || !slot->get_data().template as_opt<types::deferred_array>()) {
                    continue;
                }
                auto* const original{slot.get()};
                if (auto& forced{folder.force_deferred_array(*slot)}; &forced != original) {
                    folded.emplace_back(&slot, original);
                    folded_idxs.emplace_back(i);
                    slot.emplace(forced);
                }
            }
        }};
        fold(m.sema_side_tables.node_types.values, folded_node_idxs);
        fold(m.sema_side_tables.explicit_types.values, folded_explicit_idxs);
    }

    if (write_log) {
        // Built from exactly what this instantiation wrote, plus any deferred-array slot the fold
        // step above concretized
        const auto collect_from_log{[](const std::vector<usize>& idxs,
                                       const std::vector<usize>& extra_idxs,
                                       const auto&               live,
                                       auto&                     dst) {
            std::vector<usize> sorted{idxs};
            sorted.insert(sorted.end(), extra_idxs.begin(), extra_idxs.end());
            std::ranges::sort(sorted);
            sorted.erase(std::ranges::unique(sorted).begin(), sorted.end());
            dst.reserve(dst.size() + sorted.size());
            for (const auto i : sorted) {
                if (i < live.size() && live[i]) { dst.emplace_back(i, live[i]); }
            }
        }};
        collect_from_log(write_log->node_idxs,
                         folded_node_idxs,
                         m.sema_side_tables.node_types.values,
                         out.node_types);
        collect_from_log(write_log->explicit_idxs,
                         folded_explicit_idxs,
                         m.sema_side_tables.explicit_types.values,
                         out.explicit_types);
    } else {
        const auto collect{[](const auto& live, const auto& snap, auto& dst) {
            for (usize i{0}; i < live.size(); ++i) {
                if (!live[i]) { continue; }
                if (i >= snap.size() || !snap[i] || snap[i].get() != live[i].get()) {
                    dst.emplace_back(i, live[i]);
                }
            }
        }};
        collect(m.sema_side_tables.node_types.values, nodes, out.node_types);
        collect(m.sema_side_tables.explicit_types.values, types, out.explicit_types);
    }
    for (const auto& [slot, original] : folded) { slot->emplace(*original); }

    const auto& live_calls{m.sema_side_tables.generic_call_targets.values};
    for (usize i{0}; i < live_calls.size(); ++i) {
        if (!live_calls[i]) { continue; }
        if (i >= calls.size() || calls[i] != live_calls[i]) {
            out.call_targets.emplace_back(i, live_calls[i]);
        }
    }

    for (const auto& [idx, br] : m.if_constexpr_results) {
        const auto prev{ifs.find(idx)};
        if (prev == ifs.end() || prev->second != br) { out.if_branches.emplace_back(idx, br); }
    }
    for (const auto& [idx, arm] : m.match_arm_results) {
        const auto prev{matches.find(idx)};
        if (prev == matches.end() || prev->second != arm) { out.match_arms.emplace_back(idx, arm); }
    }

    // Reset back to the pre-res baseline so a sibling replay diffs against the same starting point
    m.if_constexpr_results = ifs;
    m.match_arm_results    = matches;
}

auto body_typing_snapshot::restore_to(mod::module& m) const -> void {
    m.sema_side_tables.node_types.values           = nodes;
    m.sema_side_tables.explicit_types.values       = types;
    m.if_constexpr_results                         = ifs;
    m.match_arm_results                            = matches;
    m.sema_side_tables.generic_call_targets.values = calls;
}

} // namespace ghoti::sema
