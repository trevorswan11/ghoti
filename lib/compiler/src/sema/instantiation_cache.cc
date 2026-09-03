#include "compiler/sema/instantiation_cache.hh"

#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/const_eval.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

auto body_typing_snapshot::diff_into(context& ctx, mod::module& m, body_type_diff& out) const
    -> void {
    std::vector<std::pair<stdx::option<type&>*, type*>> folded;
    {
        gir::const_eval folder{ctx, m};
        const auto      fold{[&](auto& slots) {
            for (auto& slot : slots) {
                if (!slot || !slot->get_data().template as_opt<types::deferred_array>()) {
                    continue;
                }
                auto* const original{slot.get()};
                if (auto& forced{folder.force_deferred_array(*slot)}; &forced != original) {
                    folded.emplace_back(&slot, original);
                    slot.emplace(forced);
                }
            }
        }};
        fold(m.sema_side_tables.node_types.values);
        fold(m.sema_side_tables.explicit_types.values);
    }

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
    for (const auto& [slot, original] : folded) { slot->emplace(*original); }

    for (const auto& [idx, br] : m.if_constexpr_results) {
        const auto prev{ifs.find(idx)};
        if (prev == ifs.end() || prev->second != br) { out.if_branches.emplace_back(idx, br); }
    }
    for (const auto& [idx, arm] : m.match_arm_results) {
        const auto prev{matches.find(idx)};
        if (prev == matches.end() || prev->second != arm) { out.match_arms.emplace_back(idx, arm); }
    }
}

} // namespace ghoti::sema
