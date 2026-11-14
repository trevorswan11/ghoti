#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>

#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "astgen_internal.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir::detail {

auto astgen::push_defer(ast::node_id id, bool is_errdefer) -> void {
    auto& state{functions_.back()};
    if (!state.allows_defer) {
        report(location_of(id),
               sema::error::ILLEGAL_TOP_LEVEL_STATEMENT,
               "Cannot have {} outside of a function's scope",
               is_errdefer ? "errdefer" : "defer");
        return;
    }
    state.defers.emplace_back<defer_entry>({
        .stmt        = id,
        .is_errdefer = is_errdefer,
        .visible_seq = seq_,
    });
}

auto astgen::has_errdefers(usize height) const -> bool {
    const auto& defers{functions_.back().defers};
    return std::ranges::any_of(defers.begin() + static_cast<idiff>(height),
                               defers.end(),
                               [](const defer_entry& entry) { return entry.is_errdefer; });
}

auto astgen::emit_defers(usize height, bool on_failure, ref residual) -> void {
    // Copied first, since a defer body's own defers push onto the same stack
    const auto&                    defers{functions_.back().defers};
    const std::vector<defer_entry> pending(defers.begin() + static_cast<idiff>(height),
                                           defers.end());
    for (const auto& entry : pending | std::views::reverse) {
        if (entry.is_errdefer && !on_failure) { continue; }
        emit_defer(entry, on_failure, residual);
    }
}

auto astgen::emit_defer(const defer_entry& entry, bool on_failure, ref residual) -> void {
    // The copy sees what was declared when the defer was written, not what came after it
    hidden_.emplace_back(entry.visible_seq, seq_);
    auto&      state{functions_.back()};
    const auto floor{std::exchange(state.target_floor, state.targets.size())};
    const auto was_in_defer{std::exchange(state.in_defer, true)};
    const auto restore{gsl::finally([&, this] {
        hidden_.pop_back();
        functions_.back().target_floor = floor;
        functions_.back().in_defer     = was_in_defer;
    })};

    if (!entry.is_errdefer) {
        DISCARD(stmt(ast_.get_as<ast::defer_stmt>(entry.stmt).deferred));
        return;
    }

    const auto&       errdefer{ast_.get_as<ast::errdefer_stmt>(entry.stmt)};
    const scope_guard scoped{*this, scope_kind::BLOCK};
    if (errdefer.capture && errdefer.capture->is<ast::identifier_expr>()) {
        DISCARD(on_failure);
        DISCARD(declare(ast_.get_as<ast::identifier_expr>(**errdefer.capture).name,
                        location_of(**errdefer.capture),
                        {.kind = binding_kind::VALUE, .value = residual}));
    }
    DISCARD(stmt(errdefer.deferred));
}

auto astgen::emit_return(ast::node_id id, ref value) -> void {
    if (!has_errdefers(0)) {
        emit_defers(0, false, {});
        DISCARD(add_un(tag::ret, value, 0, id));
        return;
    }

    // Whether the value fails is the return type's to say, so both paths are lowered
    const auto       failure{add_un(tag::ret_failure, value, 0, id)};
    const auto       failed{add_un(tag::failure_is_set, failure, 0, id)};
    std::vector<u32> on_failure;
    {
        const body_guard guard{*this, on_failure};
        const auto       residual{add_un(tag::failure_residual, failure, 0, id)};
        emit_defers(0, true, residual);
        DISCARD(add_un(tag::ret, value, 0, id));
    }
    std::vector<u32> on_success;
    {
        const body_guard guard{*this, on_success};
        emit_defers(0, false, {});
        DISCARD(add_un(tag::ret, value, 0, id));
    }
    DISCARD(add_pl(
        tag::condbr, writer().ref_field(failed).body_field(on_failure).body_field(on_success), id));
}

} // namespace ghoti::uir::detail
