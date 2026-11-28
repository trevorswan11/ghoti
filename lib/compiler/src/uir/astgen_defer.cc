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
    state.has_errdefer = state.has_errdefer || is_errdefer;
    if (is_errdefer) {
        const auto& errdefer{ast_.get_as<ast::errdefer_stmt>(id)};
        const auto  how{errdefer.modifier};
        if (how.is_mutable_ref() || how.is_mutable_ptr() || how.is_volatile() || how.is_poly()) {
            report(location_of(id),
                   sema::error::ERRDEFER_MUTABLE_CAPTURE,
                   "errdefer capture cannot have a mutable modifier");
        }
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

auto astgen::emit_defers(usize height, stdx::option<ref> residual) -> void {
    // Copied first, since a defer body's own defers push onto the same stack
    const auto&                    defers{functions_.back().defers};
    const std::vector<defer_entry> pending(defers.begin() + static_cast<idiff>(height),
                                           defers.end());
    for (const auto& entry : pending | std::views::reverse) {
        if (entry.is_errdefer && !residual) { continue; }
        emit_defer(entry, residual);
    }
}

auto astgen::emit_defer(const defer_entry& entry, stdx::option<ref> residual) -> void {
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

    // Only a failure path copies an errdefer, and every failure path has a residual
    state.copied_errdefers.emplace_back(entry.stmt.get_index());
    const auto&       errdefer{ast_.get_as<ast::errdefer_stmt>(entry.stmt)};
    const scope_guard scoped{*this, scope_kind::BLOCK};
    if (errdefer.capture && errdefer.capture->is<ast::identifier_expr>()) {
        // `|&e|` and `|^e|` borrow the residual instead of copying it
        auto       value{*residual};
        const auto how{errdefer.modifier};
        if (how.is_ref() || how.is_ptr()) {
            const u32 bits{how.is_mutable_ref() || how.is_mutable_ptr() ? flags::MUT : 0U};
            value = add_pl(how.is_ref() ? tag::ref_of : tag::address_of,
                           writer().ref_field(value).u32_field(bits),
                           **errdefer.capture);
        }

        DISCARD(declare(ast_.get_as<ast::identifier_expr>(**errdefer.capture).name,
                        location_of(**errdefer.capture),
                        {.kind = binding_kind::VALUE, .value = value}));
    }
    DISCARD(stmt(errdefer.deferred));
}

auto astgen::check_uncopied_errdefers(usize height) -> void {
    const std::vector<defer_entry> pending(functions_.back().defers.begin() +
                                               static_cast<idiff>(height),
                                           functions_.back().defers.end());
    for (const auto& entry : pending) {
        if (!entry.is_errdefer ||
            std::ranges::contains(functions_.back().copied_errdefers, entry.stmt.get_index())) {
            continue;
        }

        // The copy is never reached, so only what lowering it reports remains
        std::vector<inst_id> unused;
        const body_guard     guard{*this, unused};
        emit_defer(entry, to_ref(well_known::undefined));
    }
}

auto astgen::emit_return(ast::node_id id, ref value) -> void {
    // The value is built before the defers run
    if (!functions_.back().defers.empty() && as_well_known(value) != well_known::void_value) {
        value = add_un(tag::ret_value, value, 0, id);
    }
    if (!has_errdefers(0)) {
        emit_defers(0, stdx::none);
        DISCARD(add_un(tag::ret, value, 0, id));
        return;
    }

    // Whether the value fails is the return type's to say, so both paths are lowered
    const auto           failure{add_un(tag::ret_failure, value, 0, id)};
    const auto           failed{add_un(tag::failure_is_set, failure, 0, id)};
    std::vector<inst_id> on_failure;
    {
        const body_guard guard{*this, on_failure};
        const auto       residual{add_un(tag::failure_residual, failure, 0, id)};
        emit_defers(0, residual);
        DISCARD(add_un(tag::ret, value, 0, id));
    }
    std::vector<inst_id> on_success;
    {
        const body_guard guard{*this, on_success};
        emit_defers(0, stdx::none);
        DISCARD(add_un(tag::ret, value, 0, id));
    }
    DISCARD(add_pl(
        tag::condbr, writer().ref_field(failed).body_field(on_failure).body_field(on_success), id));
}

} // namespace ghoti::uir::detail
