#include <algorithm>
#include <array>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/instantiation_cache.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/peer_type.hh"
#include "compiler/sema/range_steps.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/sema/unwrap_shape.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"
#include "support/scope_guard.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

using namespace resolver_detail;

namespace {

// Identifiers, fields, elements, or deref off one have real storage. Anything else is an rvalue.
[[nodiscard]] auto is_lvalue_shape(const mod::module_t& module, ast::expr_handle expr) noexcept
    -> bool {
    return module.ast.get_as_opt<ast::identifier_expr>(expr) ||
           module.ast.get_as_opt<ast::dot_expr>(expr) ||
           module.ast.get_as_opt<ast::index_expr>(expr) ||
           module.ast.get_as_opt<ast::dereference_expr>(expr);
}

} // namespace

auto type_resolver::record_range_counter(usize                     loop_table,
                                         const ast::for_loop_expr& for_expr,
                                         usize                     iterable_idx) -> void {
    const auto& capture{for_expr.captures[iterable_idx]};
    if (!capture.payload.is<ast::identifier_expr>() || capture.modifier.is_ref() ||
        capture.modifier.is_ptr()) {
        return;
    }
    const auto iterable{*for_expr.iterables[iterable_idx]};
    const auto range{resolving_.ast.get_as_opt<ast::range_expr>(iterable)};
    if (!range) { return; }

    const auto fold_int{[&](ast::expr_handle bound) -> stdx::option<i128> {
        const auto folded{probe_fold(bound)};
        return folded ? folded->as_int_opt() : stdx::none;
    }};
    const auto first{range->lhs ? fold_int(*range->lhs) : stdx::option<i128>{0}};
    if (!first) { return; }

    stdx::option<i128> last;
    if (range->rhs) {
        const auto end{fold_int(*range->rhs)};
        if (!end) { return; }
        const bool inclusive{iterable.get_token_type() == syntax::token_type_t::DOT_DOT_EQ};
        const auto steps{range_steps_of(*first, *end, inclusive)};
        if (steps.count == 0) { return; }
        last = steps.last();
    } else {
        // An open range stops with its shortest sibling of known length
        stdx::option<u64> shortest;
        for (usize i{0}; i < for_expr.iterables.size(); ++i) {
            if (i == iterable_idx) { continue; }
            if (const auto len{known_length(*for_expr.iterables[i])}) {
                shortest = shortest ? std::min(*shortest, *len) : *len;
            }
        }
        if (!shortest) { return; }
        last = *first + static_cast<i128>(*shortest) - 1;
    }

    const auto& name{resolving_.ast.get_as<ast::identifier_expr>(capture.payload).name};
    range_counters_[loop_table].emplace_back(
        range_counter{.name = name, .first = *first, .last = *last});
}

auto type_resolver::adopt_counter_type(ast::node_id expr, type& target) -> bool {
    if (range_counters_.empty() || !is_integer(target.get_kind())) { return false; }
    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(expr)};
    if (!ident) { return false; }
    const auto found{ctx_.registry.lookup_with_table(table_stack_, ident->name)};
    if (!found) { return false; }
    // A closure holds its own copy of the counter, typed as stored
    const auto depth{ctx_.registry.lookup_with_depth(table_stack_, ident->name)};
    if (depth && !function_boundaries_.empty() && depth->depth < function_boundaries_.back()) {
        return false;
    }
    const auto counters{range_counters_.find(found->table_idx)};
    if (counters == range_counters_.end()) { return false; }
    const auto counter{std::ranges::find(counters->second, ident->name, &range_counter::name)};
    if (counter == counters->second.end()) { return false; }

    const auto ptr_bits{target_ptr_bits()};
    if (!comptime_int_fits(counter->first, target, ptr_bits) ||
        !comptime_int_fits(counter->last, target, ptr_bits)) {
        return false;
    }
    resolving_.set_sema_type(expr, *ctx_.pool.strip_volatile(target));
    return true;
}

namespace {

// A closure is only unsound to let escape its defining frame if it holds a ref into that frame
[[nodiscard]] auto has_dangling_capture(const types::closure_t& cl) noexcept -> bool {
    return std::ranges::any_of(cl.captures, [](const types::closure_capture& capture) {
        return capture.mode != types::capture_mode::VALUE;
    });
}

} // namespace

auto type_resolver::report_generic_escape(const type& generic_fn) -> bool {
    const auto escape{ctx_.generic_functions.get_escape(generic_fn)};
    const auto env{generic_fn.get_generic_env()};
    if (!escape || !env || escape->reported) { return false; }
    const auto& cl{env->get_data().as<types::closure_t>()};
    if (escape->erased && !cl.captures.empty()) {
        ctx_.diags.emplace_back("A capturing generic function erased into `fn(...)` borrows its "
                                "environment from this function and cannot be returned; return it "
                                "by its own type with an `auto` return instead",
                                error::ILLEGAL_CLOSURE_ESCAPE,
                                escape->at);
    } else if (has_dangling_capture(cl)) {
        ctx_.diags.emplace_back("Generic function captures a reference into its enclosing "
                                "function and cannot be returned directly; mark it 'move fn' to "
                                "capture by value instead",
                                error::ILLEGAL_CLOSURE_ESCAPE,
                                escape->at);
    } else {
        return false;
    }
    escape->reported = true;
    return true;
}

auto type_resolver::visit(ast::node_id id, const ast::do_while_loop_expr& do_while) -> void {
    PROFILE_FUNCTION();
    if (do_while.is_unrolled) { check_unrolled_loop_jumps(do_while.block); }

    // The loop itself holds the block index, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!loop_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    {
        const scope                  s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};
        const auto&                  block{resolving_.ast.get_as<ast::block_stmt>(do_while.block)};
        const mutating_context_guard cx_loop_g{in_unrolled_loop_,
                                               do_while.is_unrolled || in_unrolled_loop_};
        if (resolve_block_statements(block)) { return fail_scoped_body(); }
    }

    TRY_RESOLVE(do_while.condition);
    last_type_.emplace(loop_type);
}

namespace {

// Applies a `&`/`&mut`/`^`/`^mut`/none capture modifier, rejecting const or address-of-rvalue.
[[nodiscard]] auto resolve_capture_modifier(context&           ctx,
                                            ast::type_modifier modifier,
                                            type&              base_type,
                                            bool               container_is_const,
                                            bool               container_is_addressable,
                                            std::string_view   what,
                                            source_location    loc,
                                            bool               container_is_poly = false) noexcept
    -> stdx::result<gsl::not_null<type*>, diagnostic> {
    const bool wants_address{modifier.is_ref() || modifier.is_ptr()};
    if (wants_address && !container_is_addressable) {
        return make_sema_err(
            fmt::format("Cannot capture a temporary {} by {}; only a plain value capture is "
                        "allowed here since it has no storage of its own",
                        what,
                        modifier.is_ptr() ? "pointer" : "reference"),
            error::ILLEGAL_RVALUE_CAPTURE,
            loc);
    }

    const bool wants_mutable{modifier.is_mutable_ref() || modifier.is_mutable_ptr()};
    if (wants_mutable && container_is_const) {
        return make_sema_err(fmt::format("Cannot capture an immutable {} by mutable {}",
                                         what,
                                         modifier.is_mutable_ref() ? "reference" : "pointer"),
                             error::ASSIGNMENT_TO_CONST,
                             loc);
    }

    if (modifier.is_poly() && !container_is_poly) {
        return make_sema_err(fmt::format("A `{}` capture needs its {} reached through a "
                                         "`mut?` parameter",
                                         modifier.is_poly_ref() ? "&mut?" : "^mut?",
                                         what),
                             error::ILLEGAL_POLY_MUTABILITY,
                             loc);
    }

    const auto mutability{types::mut::from_type_modifier(modifier)};
    if (modifier.is_ref()) { return gsl::not_null{&ctx.get_reference(*mutability, base_type)}; }
    if (modifier.is_ptr()) { return gsl::not_null{&ctx.get_pointer(*mutability, base_type)}; }
    return gsl::not_null{&base_type};
}

struct unrolled_for_driver {
    bool                           is_pack{false};
    const void*                    pack{nullptr}; // the `type_resolver::pack_binding` iterated
    type*                          elem_type{nullptr};
    std::vector<gir::const_value>  elem_values; // empty for a pack driver
    stdx::option<std::string_view> name;        // unset for a `_`-discarded capture
};

// Logs one unrolled iteration's side-table writes, then hands them on to the enclosing log so an
// enclosing generic instantiation still records them
class iteration_write_log {
  public:
    explicit iteration_write_log(mod::module_t& m) noexcept
        : mod_{m}, outer_{std::exchange(m.active_write_log, stdx::option<body_write_log&>{log_})} {}

    ~iteration_write_log() noexcept {
        mod_.active_write_log = outer_;
        if (!outer_) { return; }
        const auto append{[](std::vector<usize>& dst, const std::vector<usize>& src) {
            dst.insert(dst.end(), src.begin(), src.end());
        }};
        append(outer_->node_idxs, log_.node_idxs);
        append(outer_->explicit_idxs, log_.explicit_idxs);
        append(outer_->attribute_idxs, log_.attribute_idxs);
        append(outer_->call_idxs, log_.call_idxs);
    }

    MAKE_PINNED(iteration_write_log);

    [[nodiscard]] auto get() const noexcept -> const body_write_log& { return log_; }

  private:
    body_write_log                log_;
    mod::module_t&                mod_;
    stdx::option<body_write_log&> outer_;
};

} // namespace

// Unrolls an `unroll for`, resolving the block once per iteration under its own
// `comptime_frame` slot and diffing each into a `body_type_diff` the emitter replays
auto type_resolver::resolve_unrolled_for(ast::node_id id, const ast::for_loop_expr& for_expr)
    -> void {
    PROFILE_FUNCTION();
    ASSERT(for_expr.iterables.size() == for_expr.captures.size());

    if (for_expr.iterables.empty()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "`unroll for` needs at least one driving iterable",
                             error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                             resolving_.ast.location_of(id)));
    }
    check_unrolled_loop_jumps(for_expr.block);

    auto& loop_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!loop_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    const scope     s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};
    gir::const_eval evaluator{ctx_, resolving_};
    const gir::const_eval::comptime_context_guard g{evaluator, true};
    auto& usize_type{ctx_.get_builtin_resolved_type(type_kind::USIZE)};

    // A trailing open-ended `0..` range is the companion index, not a driver - same rule as v1,
    // just checked positionally last instead of assuming exactly two iterables total.
    const bool  has_companion{[&] {
        if (for_expr.iterables.size() < 2) { return false; }
        const auto rng{resolving_.ast.get_as_opt<ast::range_expr>(*for_expr.iterables.back())};
        return rng && !rng->rhs;
    }()};
    const usize num_drivers{for_expr.iterables.size() - (has_companion ? 1 : 0)};
    if (num_drivers == 0) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "`unroll for` needs at least one driving iterable besides a companion `0..` index "
            "range",
            error::UNROLLED_LOOP_COUNT_NOT_STATIC,
            resolving_.ast.location_of(id)));
    }

    std::vector<unrolled_for_driver> drivers(num_drivers);
    stdx::opt_size                   count;
    for (usize d{0}; d < num_drivers; ++d) {
        const auto  driver_id{*for_expr.iterables[d]};
        const auto& cap{for_expr.captures[d]};
        auto&       drv{drivers[d]};
        drv.name =
            cap.payload.is<ast::identifier_expr>()
                ? stdx::option<std::string_view>{resolving_.ast
                                                     .get_as<ast::identifier_expr>(cap.payload)
                                                     .name}
                : stdx::none;

        const auto driver_ident{resolving_.ast.get_as_opt<ast::identifier_expr>(driver_id)};
        const auto pack{driver_ident ? pack_named(driver_ident->name) : nullptr};
        drv.is_pack = pack != nullptr;
        drv.pack    = pack;

        usize this_count{0};
        if (pack) {
            capture_pack_use(*pack);
            this_count = pack->element_types.size();
        } else {
            {
                const mutating_context_guard for_iter_g{in_for_iterable_, is_range(driver_id)};
                const mutating_context_guard unrolled_g{in_unrolled_for_iterable_, true};
                TRY_RESOLVE(driver_id);
            }
            // A bare identifier naming a `comptime` array or slice resolves through a `TYPE`
            // wrapped meta-type, like a `comptime T: type` parameter, so unwrap it
            auto& iterable_type{evaluator.force_deferred_array(denoted_type(*last_type_.take()))};
            resolving_.set_sema_type(driver_id, iterable_type);

            if (const auto range{resolving_.ast.get_as_opt<ast::range_expr>(driver_id)}) {
                const auto slice_data{iterable_type.get_data().as_opt<types::slice>()};
                if (!range->rhs || !slice_data) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "`unroll for`'s driving range must have a compile-time-known upper "
                        "bound",
                        error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                        resolving_.ast.location_of(driver_id)));
                }
                drv.elem_type = &slice_data->underlying;
                const auto diags_before{ctx_.diags.size()};
                const auto lo{range->lhs ? evaluator.try_eval(*range->lhs)
                                         : stdx::option<gir::const_value>{
                                               gir::const_value{u64{0}, *drv.elem_type}}};
                const auto hi{evaluator.try_eval(*range->rhs)};
                const auto lo_i{lo ? lo->as_int_opt() : stdx::none};
                const auto hi_i{hi ? hi->as_int_opt() : stdx::none};
                if (!lo_i || !hi_i) {
                    if (ctx_.diags.size() > diags_before) {
                        return last_type_.emplace(ctx_.poison_node(resolving_, id));
                    }
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "`unroll for`'s range bounds must be compile-time constant",
                        error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                        resolving_.ast.location_of(driver_id)));
                }
                const bool inclusive{driver_id.get_token_type() ==
                                     syntax::token_type_t::DOT_DOT_EQ};
                const auto steps{range_steps_of(*lo_i, *hi_i, inclusive)};
                this_count = static_cast<usize>(steps.count);
                drv.elem_values.reserve(this_count);
                for (usize k{0}; k < this_count; ++k) {
                    const auto v{evaluator.fold_binary_values(
                        steps.descends() ? syntax::token_type_t::MINUS : syntax::token_type_t::PLUS,
                        *lo,
                        gir::const_value{u64{k}},
                        driver_id)};
                    drv.elem_values.emplace_back(v.value_or(gir::const_value::make_poison()));
                }
            } else {
                const auto diags_before{ctx_.diags.size()};
                const auto folded{evaluator.try_eval(driver_id)};
                const auto arr{folded ? folded->as_opt<gir::const_array>() : stdx::none};
                const auto str{folded ? folded->as_opt<std::string>() : stdx::none};
                if (!arr && !str) {
                    if (ctx_.diags.size() > diags_before) {
                        return last_type_.emplace(ctx_.poison_node(resolving_, id));
                    }
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "`unroll for`'s iterable must be a parameter pack, a compile-time-"
                        "known range, or a compile-time-known array/slice value",
                        error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                        resolving_.ast.location_of(driver_id)));
                }
                if (arr) {
                    this_count      = arr->elements.size();
                    drv.elem_values = arr->elements;
                    const auto arr_data{iterable_type.get_data().as_opt<types::array>()};
                    const auto sl_data{iterable_type.get_data().as_opt<types::slice>()};
                    drv.elem_type = arr_data ? &arr_data->underlying
                                             : (sl_data ? &sl_data->underlying : nullptr);
                } else {
                    // String literals and compile-time string/u8 slices fold to std::string.
                    // Unpack individual character bytes as u8 compile-time constants.
                    this_count = str->size();
                    auto& u8_type{ctx_.get_int(8, false)};
                    drv.elem_values.reserve(this_count);
                    for (const char c : *str) {
                        drv.elem_values.emplace_back(static_cast<u64>(static_cast<u8>(c)), u8_type);
                    }
                    drv.elem_type = &u8_type;
                }
                if (!drv.elem_type) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "Could not determine the element type of this `unroll for` iterable",
                        error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                        resolving_.ast.location_of(driver_id)));
                }
            }
        }

        if (count && *count != this_count) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("`unroll for`'s driving iterables must all have the same length "
                            "({} vs {})",
                            *count,
                            this_count),
                error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                resolving_.ast.location_of(driver_id)));
        }
        count.emplace(this_count);
    }

    if (has_companion) {
        const auto comp_range{
            resolving_.ast.get_as_opt<ast::range_expr>(*for_expr.iterables.back())};
        if (!comp_range || comp_range->rhs) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                "`unroll for`'s companion iterable must be an open-ended index range like `0..`",
                error::UNROLLED_LOOP_COUNT_NOT_STATIC,
                resolving_.ast.location_of(*for_expr.iterables.back())));
        }
    }
    const auto companion_name{
        has_companion && for_expr.captures.back().payload.is<ast::identifier_expr>()
            ? stdx::option<std::string_view>{resolving_.ast
                                                 .get_as<ast::identifier_expr>(
                                                     for_expr.captures.back().payload)
                                                 .name}
            : stdx::none};

    const auto saved_for_gi{for_generic_instantiation_};
    const auto saved_floor{reresolve_floor_};
    for_generic_instantiation_ = true;
    reresolve_floor_.emplace(loop_type.get_symbol_table_idx());
    const auto restore_reresolve{gsl::finally([&] {
        for_generic_instantiation_ = saved_for_gi;
        reresolve_floor_           = saved_floor;
    })};

    const auto&                  block{resolving_.ast.get_as<ast::block_stmt>(for_expr.block)};
    bool                         any_poison{false};
    stdx::option<comptime_frame> carried;
    for (usize k{0}; k < *count; ++k) {
        ctx_.advance_epoch();
        const body_typing_snapshot snap{resolving_};
        const iteration_write_log  writes{resolving_};

        comptime_frame frame;
        for (usize d{0}; d < num_drivers; ++d) {
            const auto& drv{drivers[d]};
            const auto& cap{for_expr.captures[d]};
            if (drv.is_pack) {
                const auto& pack{*static_cast<const pack_binding*>(drv.pack)};
                resolving_.set_sema_type(cap.payload, *pack.element_types[k]);
            } else {
                resolving_.set_sema_type(cap.payload, *drv.elem_type);
                if (drv.name) { frame.insert_or_assign(*drv.name, drv.elem_values[k]); }
            }
            if (drv.name) { resolve_symbol_info(cap.payload, symbol_kind::VALUE); }
        }
        if (has_companion) {
            resolving_.set_sema_type(for_expr.captures.back().payload, usize_type);
            if (companion_name) {
                frame.insert_or_assign(*companion_name, gir::const_value{u64{k}, usize_type});
                resolve_symbol_info(for_expr.captures.back().payload, symbol_kind::VALUE);
            }
        }

        std::vector<std::string_view> capture_names;
        for (const auto& [name, _] : frame) { capture_names.emplace_back(name); }
        const comptime_frame_guard cfg{ctx_.comptime_binding_frames, std::move(frame)};
        const active_block_guard   guard{active_blocks_, block};
        active_blocks_.back().carried = carried;
        for (usize idx{0}; idx < block.statements.size(); ++idx) {
            active_blocks_.back().current_stmt_idx = idx;
            resolve(block.statements[idx]);
            if (last_type_->is_poison()) { any_poison = true; }
        }

        // The next iteration starts from what this one left in its `comptime let mut` locals
        active_blocks_.back().current_stmt_idx = block.statements.size();
        auto after{make_simulated_frame()};
        for (const auto name : capture_names) { after.erase(name); }
        carried.emplace(std::move(after));

        body_type_diff typing;
        snap.diff_iteration_into(ctx_, resolving_, typing, writes.get());
        const auto key{
            fmt::format("{}forloop#{}#{}",
                        typing_scope_prefix_.empty() ? std::string{} : typing_scope_prefix_ + "#",
                        id.get_index(),
                        k)};
        ctx_.instantiation_cache.set_body_type_diff(key, std::move(typing));

        std::vector<gir::const_value> cx_args;
        for (const auto& drv : drivers) {
            if (!drv.is_pack && drv.name) { cx_args.emplace_back(drv.elem_values[k]); }
        }
        if (companion_name) { cx_args.emplace_back(gir::const_value{u64{k}, usize_type}); }
        if (!cx_args.empty()) {
            ctx_.instantiation_cache.set_comptime_args(key, std::move(cx_args));
        }
    }

    if (any_poison) { return last_type_.emplace(ctx_.poison_node(resolving_, id)); }
    resolving_.set_sema_type(id, loop_type);
    last_type_.emplace(resolving_.get_sema_type(id));
}

auto type_resolver::visit(ast::node_id id, const ast::for_loop_expr& for_expr) -> void {
    PROFILE_FUNCTION();
    ASSERT(for_expr.iterables.size() == for_expr.captures.size());
    if (for_expr.is_unrolled) { return resolve_unrolled_for(id, for_expr); }

    // The loop itself holds the block index which houses captures, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!loop_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};

        // A `for (arr, lo..) |v, i|` open-upper range needs a sibling array/slice to stop the loop.
        bool has_open_upper_range{false};
        bool has_bounding_iterable{false};
        for (const auto& iterable : for_expr.iterables) {
            const auto rng{resolving_.ast.get_as_opt<ast::range_expr>(iterable)};
            if (rng && !rng->rhs) {
                has_open_upper_range = true;
            } else {
                has_bounding_iterable = true;
            }
        }
        if (has_open_upper_range && !has_bounding_iterable) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                "An open-ended range loop needs an array or slice iterable to bound it, e.g. "
                "`for (arr, 0..) |v, i|`",
                error::ILLEGAL_OPEN_RANGE,
                resolving_.ast.location_of(id)));
        }

        // The captures must be paired with the iterables inner types (shallow type check)
        for (const auto& [capture, iterable] :
             std::views::zip(for_expr.captures, for_expr.iterables)) {
            {
                const mutating_context_guard for_iter_g{in_for_iterable_, is_range(iterable)};
                TRY_RESOLVE(iterable);
            }
            auto& iterable_type{*last_type_.take()};
            resolving_.set_sema_type(iterable, iterable_type);

            // Assign types unconditionally since ignoring discards saves no space
            auto& iterable_data{iterable_type.get_data()};
            type* elem_type{nullptr};
            if (const auto array{iterable_data.as_opt<types::array>()}) {
                elem_type = &array->underlying;
            } else if (const auto deferred{iterable_data.as_opt<types::deferred_array>()}) {
                elem_type = &deferred->underlying;
            } else if (const auto slice{iterable_data.as_opt<types::slice>()}) {
                elem_type = &slice->underlying;
            } else {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Iterables may only be arrays or slices; found '{}'",
                                ctx_.type_display_name(iterable_type)),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(iterable)));
            }

            auto cap_result{resolve_capture_modifier(
                ctx_,
                capture.modifier,
                *elem_type,
                container_element_mutability(iterable_type) == types::mut::CONSTANT,
                is_lvalue_shape(resolving_, iterable),
                "array or slice",
                resolving_.ast.location_of(capture.payload),
                iterable_type.is_poly() || place_mutability(iterable) == types::mut::POLY)};
            if (!cap_result) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, std::move(cap_result).error()));
            }
            resolving_.set_sema_type(capture.payload, **cap_result);

            if (capture.payload.is<ast::identifier_expr>()) {
                resolve_symbol_info(capture.payload, symbol_kind::VALUE);
            }
        }
        range_counters_.erase(loop_type.get_symbol_table_idx());
        for (usize i{0}; i < for_expr.iterables.size(); ++i) {
            record_range_counter(loop_type.get_symbol_table_idx(), for_expr, i);
        }
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(for_expr.block)};
        if (resolve_block_statements(block)) { return fail_scoped_body(); }
    }

    if (for_expr.non_break) {
        // A loop's `else` yields its value like an `if` arm, discarded only with the loop
        const mutating_context_guard branch_g{in_expr_branch_, true};
        const mutating_context_guard unused_g{arm_of_unused_,
                                              unused_value_nodes_.contains(id.get_index())};
        TRY_RESOLVE(*for_expr.non_break);
    }
    resolving_.set_sema_type(
        id, loop_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : loop_type);
    last_type_.emplace(resolving_.get_sema_type(id));
}

auto type_resolver::fail_scoped_body() -> void { last_type_.emplace(ctx_.get_poison()); }

namespace {

// Whether `stmt` always leaves its block: a `return`, `break`, or `continue`, or a block ending in
// one
[[nodiscard]] auto always_exits(const ast::AST& tree, ast::node_id stmt) -> bool {
    if (tree.get_as_opt<ast::return_stmt>(stmt) || tree.get_as_opt<ast::break_stmt>(stmt) ||
        tree.get_as_opt<ast::continue_stmt>(stmt)) {
        return true;
    }
    if (const auto block{tree.get_as_opt<ast::block_stmt>(stmt)}) {
        return !block->statements.empty() && always_exits(tree, *block->statements.back());
    }
    return false;
}

} // namespace

auto type_resolver::resolve_block_statements(const ast::block_stmt& block) -> bool {
    const active_block_guard guard{active_blocks_, block};
    for (usize idx{0}; idx < block.statements.size(); ++idx) {
        active_blocks_.back().current_stmt_idx = idx;
        resolve(block.statements[idx]);
        if (last_type_->is_poison()) { return true; }
        // An `if comptime` that took an arm which always leaves makes the rest of the block dead,
        // just as the emitter stops there
        if (folded_if_exits(block.statements[idx])) { break; }
    }
    return false;
}

auto type_resolver::folded_if_exits(ast::node_id stmt) const -> bool {
    const auto expr_stmt{resolving_.ast.get_as_opt<ast::expr_stmt>(stmt)};
    if (!expr_stmt) { return false; }
    const ast::node_id if_id{expr_stmt->expression};
    const auto         if_expr{resolving_.ast.get_as_opt<ast::if_expr>(if_id)};
    if (!if_expr || !if_expr->comptime_condition) { return false; }
    const auto branch{resolving_.get_if_branch_opt(if_id.get_index())};
    if (!branch) { return false; }
    if (*branch == mod::if_branch::CONSEQUENCE) {
        return always_exits(resolving_.ast, if_expr->consequence);
    }
    return if_expr->alternate && always_exits(resolving_.ast, *if_expr->alternate);
}

auto type_resolver::visit(ast::node_id id, const ast::if_expr& if_expr) -> void {
    PROFILE_FUNCTION();
    // `if comptime { a } else { b }`: both arms are live, each in its own evaluation context
    if (if_expr.is_evaluation_context_branch()) { return resolve_if_arms(id, if_expr); }
    TRY_RESOLVE(*if_expr.condition);
    if (reject_type_condition(*if_expr.condition)) { return; }
    if (const auto poisoned{resolve_unwrap_captures(id,
                                                    *if_expr.condition,
                                                    if_expr.payload_capture,
                                                    if_expr.else_capture,
                                                    capture_scope(if_expr.payload_capture),
                                                    "if",
                                                    if_expr.comptime_condition)}) {
        return last_type_.emplace(*poisoned);
    }

    // The template pass has no real impl-param values. Leave an `if comptime` unresolved here so
    // config-specific dead arms never poison. `resolve_param_impl_bodies` folds it per instance.
    if (if_expr.comptime_condition && building_param_template_) {
        auto& void_t{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        resolving_.set_sema_type(id, void_t);
        return last_type_.emplace(void_t);
    }

    if (if_expr.comptime_condition && !in_unrolled_loop_) {
        // Install simulated comptime_frame so condition folding observes preceding `comptime let
        // mut` mutations.
        const comptime_frame_guard sim_guard{ctx_.comptime_binding_frames, make_simulated_frame()};
        gir::const_eval            evaluator{ctx_, resolving_};
        const gir::const_eval::comptime_context_guard g{evaluator, true};
        if (const auto cond_cv{evaluator.try_eval(*if_expr.condition)}) {
            if (const auto folded{cond_cv->as_opt<bool>()}) {
                const auto arm_value_type{[&](ast::stmt_handle arm) -> type& {
                    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(arm)}) {
                        if (const auto t{resolving_.get_sema_type_opt(es->expression)}) {
                            return *t;
                        }
                    }
                    return *last_type_;
                }};

                stdx::option<type&>          live_type;
                const mutating_context_guard branch_g{in_expr_branch_, true};
                if (*folded) {
                    TRY_RESOLVE(if_expr.consequence);
                    live_type.emplace(arm_value_type(if_expr.consequence));
                } else if (if_expr.alternate) {
                    TRY_RESOLVE(*if_expr.alternate);
                    live_type.emplace(arm_value_type(*if_expr.alternate));
                }
                last_type_.reset();
                auto& node_type{live_type ? *live_type
                                          : ctx_.get_builtin_resolved_type(type_kind::VOID_)};
                // Per-instantiation verdicts are captured and replayed by instantiate_generic.
                resolving_.if_comptime_results.insert_or_assign(
                    id.get_index(),
                    *folded ? mod::if_branch::CONSEQUENCE : mod::if_branch::ALTERNATE);
                resolving_.set_sema_type(id, node_type);
                last_type_.emplace(node_type);
                return;
            }
        }
    }

    resolve_if_arms(id, if_expr);
}

auto type_resolver::in_dead_arm() -> bool {
    gir::const_eval evaluator{ctx_, resolving_};
    return std::ranges::any_of(enclosing_arms_, [&](const enclosing_arm& arm) {
        if (arm.match) {
            const auto selected{evaluator.selected_match_arm(*arm.match)};
            return selected && *selected != arm.arm_idx;
        }
        const auto cv{evaluator.try_eval(*arm.condition)};
        const auto taken{cv ? cv->as_opt<bool>() : stdx::none};
        return taken && *taken != arm.consequence;
    });
}

auto type_resolver::capture_scope(const stdx::option<ast::capture_t>& capture) const
    -> stdx::opt_size {
    if (!capture || !capture->payload.is<ast::identifier_expr>()) { return stdx::none; }
    return resolving_.sema_side_tables.capture_scopes[*capture->payload];
}

auto type_resolver::resolve_unwrap_captures(ast::node_id                        id,
                                            ast::expr_handle                    condition,
                                            const stdx::option<ast::capture_t>& payload,
                                            const stdx::option<ast::capture_t>& residual,
                                            stdx::opt_size                      payload_scope,
                                            std::string_view                    construct,
                                            bool comptime) -> stdx::option<type&> {
    const auto cond_type{resolving_.get_sema_type_opt(condition)};
    if (!cond_type || cond_type->is_poison()) { return stdx::none; }
    const auto shape{unwrap_shape_of(ctx_, *cond_type)};
    const auto cond_loc{resolving_.ast.location_of(condition)};
    const auto cond_name{ctx_.type_display_name(*cond_type)};

    if (!payload) {
        if (!shape) { return stdx::none; }
        return ctx_.poison_node(resolving_,
                                id,
                                fmt::format("`{0}` on a '{1}' unwraps it, so it needs a capture; "
                                            "write `{0} (...) |_|` to ignore the payload",
                                            construct,
                                            cond_name),
                                error::ILLEGAL_UNWRAP_CAPTURE,
                                cond_loc);
    }
    if (!shape) {
        return ctx_.poison_node(resolving_,
                                id,
                                fmt::format("A capture on `{}` needs a condition that "
                                            "implements 'builtin.Unwrappable'; '{}' does not",
                                            construct,
                                            cond_name),
                                error::ILLEGAL_UNWRAP_CAPTURE,
                                cond_loc);
    }

    // A compile-time construct's capture is a value with no runtime place to refer to
    if (comptime &&
        (!payload->modifier.is_value() || (residual && !residual->modifier.is_value()))) {
        return ctx_.poison_node(resolving_,
                                id,
                                fmt::format("A capture on `{} comptime` is a compile-time "
                                            "value, so it takes no `&` or `^` modifier",
                                            construct),
                                error::ILLEGAL_UNWRAP_CAPTURE,
                                resolving_.ast.location_of(*payload->payload));
    }

    // The same capture forms as a `match` arm. A temporary condition gets a slot of its own
    const auto mutability{place_mutability(condition)};
    auto       payload_type{resolve_capture_modifier(ctx_,
                                                     payload->modifier,
                                                     *shape->output_type,
                                                     mutability != types::mut::MUTABLE,
                                                     true,
                                                     "value",
                                                     resolving_.ast.location_of(*payload->payload),
                                                     mutability == types::mut::POLY)};
    if (!payload_type) { return ctx_.poison_node(resolving_, id, std::move(payload_type).error()); }
    if (payload->payload.is<ast::identifier_expr>()) {
        resolving_.set_sema_type(*payload->payload, **payload_type);
        if (payload_scope) {
            const scope s{table_stack_, *payload_scope, table_idx_};
            resolve_symbol_info(ast::identifier_handle{*payload->payload}, symbol_kind::VALUE);
        }
    }

    if (!residual) { return stdx::none; }
    const auto residual_loc{resolving_.ast.location_of(*residual->payload)};
    if (shape->residual_is_void) {
        return ctx_.poison_node(
            resolving_,
            id,
            fmt::format("'{}' carries nothing when it doesn't unwrap, so its `else` has no value "
                        "to capture",
                        cond_name),
            error::ILLEGAL_UNWRAP_CAPTURE,
            residual_loc);
    }
    if (!residual->modifier.is_value()) {
        return ctx_.poison_node(resolving_,
                                id,
                                "An `else` capture is the residual itself, so it takes no `&` or "
                                "`^` modifier",
                                error::ILLEGAL_UNWRAP_CAPTURE,
                                residual_loc);
    }
    if (residual->payload.is<ast::identifier_expr>()) {
        resolving_.set_sema_type(*residual->payload, *shape->residual_type);
        if (const auto residual_scope{capture_scope(residual)}) {
            const scope s{table_stack_, *residual_scope, table_idx_};
            resolve_symbol_info(ast::identifier_handle{*residual->payload}, symbol_kind::VALUE);
        }
    }
    return stdx::none;
}

auto type_resolver::resolve_if_arms(ast::node_id id, const ast::if_expr& if_expr) -> void {
    const mutating_context_guard branch_g{in_expr_branch_, true};
    const mutating_context_guard unused_g{arm_of_unused_,
                                          unused_value_nodes_.contains(id.get_index())};
    // `if comptime { } else { }` has no condition and keeps both arms live
    const bool track_arms{if_expr.condition.has_value()};
    {
        stdx::option<scope_guard<std::vector<enclosing_arm>>> arm_g;
        if (track_arms) {
            arm_g.emplace(enclosing_arms_,
                          enclosing_arm{.condition = if_expr.condition, .consequence = true});
        }
        stdx::option<scope> capture_s;
        if (const auto idx{capture_scope(if_expr.payload_capture)}) {
            capture_s.emplace(table_stack_, *idx, table_idx_);
        }
        TRY_RESOLVE(if_expr.consequence);
    }

    // What an arm yields: its expression's value, or nothing at all when it jumps away
    const auto arm_value{[&](ast::stmt_handle arm, type& resolved) -> stdx::option<result_value> {
        if (const auto expr{resolving_.ast.get_as_opt<ast::expr_stmt>(arm)}) {
            if (const auto expr_type{resolving_.get_sema_type_opt(expr->expression)}) {
                return result_value{expr_type.get(), *expr->expression};
            }
            return stdx::none;
        }
        if (resolved.get_kind() == type_kind::NORETURN) { return result_value{&resolved, *arm}; }
        return stdx::none;
    }};

    auto*      branch_type{last_type_.take()};
    const auto cons_value{arm_value(if_expr.consequence, *branch_type)};
    if (if_expr.alternate) {
        stdx::option<scope_guard<std::vector<enclosing_arm>>> arm_g;
        if (track_arms) {
            arm_g.emplace(enclosing_arms_,
                          enclosing_arm{.condition = if_expr.condition, .consequence = false});
        }
        stdx::option<scope> capture_s;
        if (const auto idx{capture_scope(if_expr.else_capture)}) {
            capture_s.emplace(table_stack_, *idx, table_idx_);
        }
        TRY_RESOLVE(*if_expr.alternate);
        const auto alt_value{arm_value(*if_expr.alternate, *last_type_)};
        // One arm naming a type and the other a value has no single result
        if (cons_value && alt_value && cons_value->type->get_kind() != type_kind::NORETURN &&
            alt_value->type->get_kind() != type_kind::NORETURN) {
            const auto cons_nature{operand_nature(ast::expr_handle{cons_value->at})};
            const auto alt_nature{operand_nature(ast::expr_handle{alt_value->at})};
            if ((cons_nature == operand_nature_t::TYPE && alt_nature == operand_nature_t::VALUE) ||
                (cons_nature == operand_nature_t::VALUE && alt_nature == operand_nature_t::TYPE)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "One branch of this `if` is a type and the other a value",
                                     error::TYPE_USED_AS_VALUE,
                                     resolving_.ast.location_of(id)));
            }
        }
        if (cons_value && alt_value && !cons_value->type->is_poison() &&
            !alt_value->type->is_poison() && cons_value->type->get_kind() != type_kind::VOID_) {
            const std::array arms{*cons_value, *alt_value};
            branch_type = &result_peer(id, arms);
        }
    }
    resolving_.set_sema_type(id, *branch_type);
    last_type_.emplace(*branch_type);
}

auto type_resolver::visit(ast::node_id id, const ast::infinite_loop_expr& loop) -> void {
    PROFILE_FUNCTION();
    if (loop.is_unrolled) { check_unrolled_loop_jumps(loop.block); }
    auto& loop_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!loop_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};

    // Just an abridged normal loop handler
    const auto&                  block{resolving_.ast.get_as<ast::block_stmt>(loop.block)};
    const mutating_context_guard cx_loop_g{in_unrolled_loop_,
                                           loop.is_unrolled || in_unrolled_loop_};
    if (resolve_block_statements(block)) { return fail_scoped_body(); }
    last_type_.emplace(loop_type);
}

auto type_resolver::visit(ast::node_id id, const ast::label_expr& label) -> void {
    PROFILE_FUNCTION();
    auto&      label_type{resolving_.get_sema_type(id)};
    const auto table_opt{resolving_.get_symbol_table_opt(id)};
    const auto table_idx{
        table_opt
            ? *table_opt
            : (label_type.has_symbol_table_idx() ? label_type.get_symbol_table_idx() : usize{0})};
    resolving_.set_symbol_table(id, table_idx);
    const scope                     s{table_stack_, table_idx, table_idx_};
    const comptime_evaluation_scope cx_scope{ctx_, label.is_comptime(resolving_.ast)};

    // The label symbol is shared across generic instantiations, so drop yields from a prior pass
    if (label.name) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(*label.name)};
        if (auto sym{ctx_.registry.lookup(table_stack_, ident.name)};
            sym && sym->get_kind() == symbol_kind::LABEL) {
            symbols::label::from(*sym).clear_yield_types();
        }
    }

    // Resolve the body but cache the label's type so the result can bind to the label
    TRY_RESOLVE(*label.body);
    if (label.name) {
        auto sym{resolve_symbol_info(*label.name, stdx::none)};
        if (!sym) { return; }
        auto& label_data{symbols::label::from(*sym)};

        // A labeled loop also yields its `else` value when it runs out without a `break`
        const auto loop_else{[&]() -> stdx::option<ast::stmt_handle> {
            if (const auto w{resolving_.ast.get_as_opt<ast::while_loop_expr>(*label.body)}) {
                return w->non_break;
            }
            if (const auto f{resolving_.ast.get_as_opt<ast::for_loop_expr>(*label.body)}) {
                return f->non_break;
            }
            return stdx::none;
        }()};
        if (loop_else) {
            if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(*loop_else)}) {
                if (const auto t{resolving_.get_sema_type_opt(es->expression)};
                    t && t->get_kind() != type_kind::NORETURN &&
                    t->get_kind() != type_kind::VOID_) {
                    label_data.add_yield_type(*t);
                }
            }
        }

        // Labels may not be used outside of continues which don't push void
        if (!label_data.has_yield_types()) {
            label_data.add_yield_type(ctx_.get_builtin_resolved_type(type_kind::VOID_));
        }

        // The last type inherits the result type to help propagation of poison
        // Every value the label is broken with meets at one type
        std::vector<result_value> yields;
        for (const auto yielded : label_data.get_yield_types()) {
            yields.emplace_back(yielded, id);
        }
        auto& result_type{comptime_numeric_view(yields.size() > 1 ? result_peer(id, yields)
                                                                  : *yields.front().type)};
        ASSERT(result_type.is_resolved(), "The label's inner type should've been resolved");
        label_type.resolve_if<type::data_t>(result_type.get_data());
        resolving_.set_sema_type(*label.name, result_type);
        resolving_.set_sema_type(id, result_type);
        last_type_.emplace(result_type);
    } else {
        // `comptime { ...; return r; }` never finishes, so it takes no part in its context's type
        const bool exits{label.body.is<ast::block_stmt>() &&
                         always_exits(resolving_.ast, *label.body)};
        auto&      result_type{
            ctx_.get_builtin_resolved_type(exits ? type_kind::NORETURN : type_kind::VOID_)};
        label_type.resolve_if<type::data_t>(result_type.get_data());
        resolving_.set_sema_type(id, result_type);
        last_type_.emplace(result_type);
    }
}

namespace {

// Collects potential duplicate implicit access match arms for the structural type
auto gather_arm_duplicates(gsl::span<const ast::match_expr::arm> arms,
                           mod::module_t&                        resolving,
                           type_resolver::structural_validator&  validator,
                           bool require_implicit_access) -> stdx::option<diagnostic> {
    for (const auto& arm : arms) {
        for (const auto& pattern : arm.patterns) {
            if (pattern.is<ast::discarded>()) { continue; }

            // It's only possible to verify access expressions
            const auto pattern_node{resolving.ast.get_as_opt<ast::implicit_access_expr>(pattern)};
            if (!pattern_node) {
                if (require_implicit_access) {
                    return diagnostic{
                        "Match arm may only have an implicit access pattern in this context",
                        error::ILLEGAL_MATCH_PATTERN,
                        resolving.ast.location_of(pattern)};
                }
                continue;
            }

            const auto& ident{resolving.ast.get_as<ast::identifier_expr>(pattern_node->member)};
            if (!validator.seen.insert(ident.name).second) {
                validator.duplicates.emplace_back(ident.name);
            }
            validator.provided.emplace_back(ident.name);
        }
    }
    return stdx::none;
}

} // namespace

auto type_resolver::validate_enum_arms(ast::node_id           match_id,
                                       const ast::match_expr& match,
                                       type& enum_type) -> stdx::option<diagnostic> {
    enum_validator_.clear();

    if (const auto e_data{enum_type.get_data().as_opt<types::enum_t>()};
        e_data && e_data->non_exhaustive && !match.catch_all_idx) {
        return diagnostic{"Match expressions over non-exhaustive enums must have a catch all arm",
                          error::ILLEGAL_MATCH_PATTERN,
                          resolving_.ast.location_of(match_id)};
    }

    // Track seen and duplicate variants in the match arms
    const auto diag{gather_arm_duplicates(match.arms, resolving_, enum_validator_, false)};
    ASSERT(!diag, "Enum validation should not return a diagnostic");
    if (!enum_validator_.duplicates.empty()) {
        return diagnostic{
            fmt::format("Match expression contains duplicate enumeration{}: {}",
                        string_utils::plural_suffix(enum_validator_.duplicates.size()),
                        fmt::join(enum_validator_.duplicates, ", ")),
            error::DUPLICATE_ENUMERATION,
            resolving_.ast.location_of(match_id)};
    }

    // Check for missing variants only if there's no catch-all
    if (!match.catch_all_idx) {
        if (const auto e_data{enum_type.get_data().as_opt<types::enum_t>()}) {
            const auto& enclosing{e_data->enclosing};
            for (const auto& e : e_data->ast_enumerations) {
                const auto& variant_node{enclosing.ast.get_as<ast::identifier_expr>(e.name)};
                if (!enum_validator_.seen.contains(variant_node.name)) {
                    enum_validator_.missings.emplace_back(variant_node.name);
                }
            }
        }

        if (!enum_validator_.missings.empty()) {
            return diagnostic{
                fmt::format("Match expression is non-exhaustive; missing enumeration{}: {}",
                            string_utils::plural_suffix(enum_validator_.missings.size()),
                            fmt::join(enum_validator_.missings, ", ")),
                error::MISSING_FIELD,
                resolving_.ast.location_of(match_id)};
        }
    }

    // Check for unknown/extra variants
    const auto  enum_table_idx{enum_type.get_symbol_table_idx()};
    const auto& enum_table{ctx_.registry.get(enum_table_idx)};
    for (const auto name : enum_validator_.provided) {
        if (!enum_table.has(name)) { enum_validator_.unknowns.emplace_back(name); }
    }

    if (!enum_validator_.unknowns.empty()) {
        return diagnostic{fmt::format("Match expression contains unknown enumeration{}: {}",
                                      string_utils::plural_suffix(enum_validator_.unknowns.size()),
                                      fmt::join(enum_validator_.unknowns, ", ")),
                          error::UNKNOWN_ENUMERATION,
                          resolving_.ast.location_of(match_id)};
    }
    return stdx::none;
}

auto type_resolver::validate_union_arms(ast::node_id           match_id,
                                        const ast::match_expr& match,
                                        type& union_type) -> stdx::option<diagnostic> {
    union_validator_.clear();
    const auto union_data_opt{union_type.get_data().as_opt<types::union_t>()};
    if (!union_data_opt) { return stdx::none; }
    const auto& union_data{*union_data_opt};

    // An untagged (extern) union has no runtime discriminant to check a field against
    if (union_data.is_untagged) {
        return diagnostic{"Cannot match on an untagged union; it has no runtime tag",
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(match.matcher)};
    }

    // Track seen and duplicate fields in the match arms
    if (auto diag{gather_arm_duplicates(match.arms, resolving_, union_validator_, true)}; diag) {
        return diag;
    }

    if (!union_validator_.duplicates.empty()) {
        return diagnostic{
            fmt::format("Match expression contains duplicate union field{}: {}",
                        string_utils::plural_suffix(union_validator_.duplicates.size()),
                        fmt::join(union_validator_.duplicates, ", ")),
            error::DUPLICATE_FIELD,
            resolving_.ast.location_of(match_id)};
    }

    // Check for missing fields only if there's a missing catch all
    if (!match.catch_all_idx) {
        const auto& enclosing{union_data.enclosing};
        for (const auto& [ident, _1, _2] : union_data.ast_fields) {
            const auto& field_node{enclosing.ast.get_as<ast::identifier_expr>(ident)};
            if (!union_validator_.seen.contains(field_node.name)) {
                union_validator_.missings.emplace_back(field_node.name);
            }
        }

        if (!union_validator_.missings.empty()) {
            return diagnostic{
                fmt::format("Match expression is non-exhaustive; missing union field{}: {}",
                            string_utils::plural_suffix(union_validator_.missings.size()),
                            fmt::join(union_validator_.missings, ", ")),
                error::MISSING_FIELD,
                resolving_.ast.location_of(match_id)};
        }
    }

    // Check for unknown fields
    const auto  union_table_idx{union_type.get_symbol_table_idx()};
    const auto& union_table{ctx_.registry.get(union_table_idx)};
    for (const auto name : union_validator_.provided) {
        if (!union_table.has(name)) { union_validator_.unknowns.emplace_back(name); }
    }

    if (!union_validator_.unknowns.empty()) {
        return diagnostic{fmt::format("Match expression contains unknown union field{}: {}",
                                      string_utils::plural_suffix(union_validator_.unknowns.size()),
                                      fmt::join(union_validator_.unknowns, ", ")),
                          error::UNKNOWN_FIELD,
                          resolving_.ast.location_of(match_id)};
    }
    return stdx::none;
}

auto type_resolver::resolve_type_match(ast::node_id           id,
                                       const ast::match_expr& match,
                                       type&                  matcher_type) -> void {
    PROFILE_FUNCTION();

    if (!match.catch_all_idx) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "A 'match' on a type requires a catch-all '_' arm",
                             error::ILLEGAL_MATCH_PATTERN,
                             resolving_.ast.location_of(id)));
    }

    gir::const_eval evaluator{ctx_, resolving_};

    // Resolve the scrutinee to a concrete type when it is known in this context
    stdx::option<type&> concrete;
    if (auto& m{denoted_type(matcher_type)};
        m.is_resolved() && m.get_kind() != type_kind::TYPE && m.get_kind() != type_kind::AUTO) {
        concrete.emplace(m);
    } else if (const auto cv{evaluator.try_eval(match.matcher)};
               cv && cv->is<stdx::option<sema::type&>>()) {
        if (const stdx::option<sema::type&> t{cv->as<stdx::option<sema::type&>>()};
            t && t->is_resolved()) {
            concrete.emplace(denoted_type(*t));
        }
    }

    // Resolve/validate every arm pattern and pick the arm a concrete scrutinee selects.
    stdx::opt_size selected;
    for (usize i{0}; i < match.arms.size(); ++i) {
        const auto& arm{match.arms[i]};
        auto&       arm_table_type{resolving_.get_sema_type(arm)};
        const scope arm_scope{table_stack_, arm_table_type.get_symbol_table_idx(), table_idx_};

        if (arm.capture) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "A 'match' on a type cannot bind a capture",
                                 error::ILLEGAL_MATCH_PATTERN,
                                 resolving_.ast.location_of(*arm.capture->payload)));
        }
        if (match.catch_all_idx && i == *match.catch_all_idx) { continue; }

        for (const auto& pattern : arm.patterns) {
            resolve(pattern);
            if (last_type_->is_poison()) { return resolving_.set_sema_type(id, *last_type_); }
            auto& pat_type{*last_type_.take()};

            // A pattern that folds to a concrete value is not a type.
            if (const auto pat_cv{evaluator.try_eval(pattern)};
                pat_cv && !pat_cv->is_poison() && !pat_cv->is<stdx::option<sema::type&>>()) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A 'match' on a type expects every arm pattern to be a type",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(pattern)));
            }

            if (concrete && !selected) {
                if (auto& pat_concrete{denoted_type(pat_type)};
                    pat_concrete.is_resolved() &&
                    sema::is_same_unqualified(*concrete, pat_concrete)) {
                    selected.emplace(i);
                }
            }
        }
    }

    if (concrete && !selected) { selected = match.catch_all_idx; }
    if (!selected) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "'match' on type has no arm matching the scrutinee and no '_' arm",
                             error::COMPTIME_EVALUATION_FAILED,
                             resolving_.ast.location_of(id)));
    }

    // Type-check only the live arm's body
    const usize live_arm{*selected};
    const auto& live{match.arms[live_arm]};
    {
        auto&       live_table_type{resolving_.get_sema_type(live)};
        const scope live_scope{table_stack_, live_table_type.get_symbol_table_idx(), table_idx_};
        const mutating_context_guard branch_g{in_expr_branch_, true};
        TRY_RESOLVE(live.dispatch);
    }

    type* result_type{&ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(live.dispatch)}) {
        if (const auto inner{resolving_.get_sema_type_opt(es->expression)}) {
            if (!inner->is_poison() && inner->get_kind() != type_kind::VOID_) {
                result_type = inner.get();
            }
        }
    }

    if (selected) { resolving_.match_arm_results.insert_or_assign(id.get_index(), *selected); }
    resolving_.set_sema_type(id, *result_type);
    last_type_.emplace(*result_type);
}

auto type_resolver::resolve_comptime_match(ast::node_id           id,
                                           const ast::match_expr& match,
                                           type&                  matcher_type) -> void {
    PROFILE_FUNCTION();

    const comptime_frame_guard sim_guard{ctx_.comptime_binding_frames, make_simulated_frame()};
    gir::const_eval            evaluator{ctx_, resolving_};
    const gir::const_eval::comptime_context_guard g{evaluator, true};
    const auto                                    scrutinee{evaluator.try_eval(match.matcher)};
    if (!scrutinee || scrutinee->is_poison()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "'match comptime' requires a compile-time-known scrutinee",
                             error::COMPTIME_EVALUATION_FAILED,
                             resolving_.ast.location_of(match.matcher)));
    }

    stdx::opt_size selected;
    for (usize i{0}; i < match.arms.size() && !selected; ++i) {
        if (match.catch_all_idx && i == *match.catch_all_idx) { continue; }
        for (const auto& pattern : match.arms[i].patterns) {
            if (evaluator.arm_pattern_matches(pattern, *scrutinee)) {
                selected.emplace(i);
                break;
            }
        }
    }
    if (!selected) { selected = match.catch_all_idx; }
    if (!selected) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "'match comptime' has no arm matching the scrutinee and no '_' arm",
                             error::COMPTIME_EVALUATION_FAILED,
                             resolving_.ast.location_of(id)));
    }

    // Only the live arm is type-checked. The rest is dead code.
    const auto& live{match.arms[*selected]};
    {
        auto&       live_table_type{resolving_.get_sema_type(live)};
        const scope live_scope{table_stack_, live_table_type.get_symbol_table_idx(), table_idx_};

        comptime_frame frame;
        if (live.capture && live.capture->payload.is<ast::identifier_expr>()) {
            if (scrutinee->is<stdx::option<sema::type&>>()) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "'match comptime' on a type value cannot bind a capture",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*live.capture->payload)));
            }
            if (!live.capture->modifier.is_value()) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "'match comptime' captures cannot use a reference or pointer modifier",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving_.ast.location_of(*live.capture->payload)));
            }

            auto& effective_matcher{denoted_type(matcher_type)};
            type* cap_type{&effective_matcher};
            if (const auto ud{effective_matcher.get_data().as_opt<types::union_t>()}) {
                if (const auto ia{resolving_.ast.get_as_opt<ast::implicit_access_expr>(
                        *live.primary_pattern())}) {
                    const auto& table{ctx_.registry.get(effective_matcher.get_symbol_table_idx())};
                    const auto& pident{resolving_.ast.get_as<ast::identifier_expr>(ia->member)};
                    cap_type = &ud->type_at(table.get_proxy(pident.name).index);
                }
            }
            resolving_.set_sema_type(*live.capture->payload, *cap_type);
            resolve_symbol_info(live.capture->payload, symbol_kind::VALUE);

            // Give the capture the same `comptime_frame` binding an `unroll for`/`while`
            // capture already gets
            const auto& cap_ident{
                resolving_.ast.get_as<ast::identifier_expr>(*live.capture->payload)};
            const auto un{scrutinee->as_opt<gir::const_union>()};
            if (un && !live.primary_pattern().is<ast::discarded>()) {
                frame.insert_or_assign(cap_ident.name,
                                       !un->payload.empty() ? un->payload.front() : *scrutinee);
            } else {
                frame.insert_or_assign(cap_ident.name, *scrutinee);
            }
        }

        const mutating_context_guard branch_g{in_expr_branch_, true};
        const comptime_frame_guard   cfg{ctx_.comptime_binding_frames, std::move(frame)};
        TRY_RESOLVE(live.dispatch);
    }

    type* result_type{&ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    if (const auto es{resolving_.ast.get_as_opt<ast::expr_stmt>(live.dispatch)}) {
        if (const auto inner{resolving_.get_sema_type_opt(es->expression)}) {
            if (!inner->is_poison() && inner->get_kind() != type_kind::VOID_) {
                result_type = inner.get();
            }
        }
    } else if (always_exits(resolving_.ast, *live.dispatch)) {
        // `=> comptime { ...; return r; }` leaves the function instead of yielding
        result_type = &ctx_.get_builtin_resolved_type(type_kind::NORETURN);
    }

    resolving_.match_arm_results.insert_or_assign(id.get_index(), *selected);
    resolving_.set_sema_type(id, *result_type);
    last_type_.emplace(*result_type);
}

namespace {

// A constant scalar pattern or range, closed on both ends, for overlap detection
struct arm_interval {
    i64                       lo;
    i64                       hi;
    ast::match_pattern_handle pat;
};

// The first pattern covering a value an earlier one already covers
[[nodiscard]] auto first_overlapping_pattern(gsl::span<const arm_interval> intervals)
    -> stdx::option<ast::match_pattern_handle> {
    for (usize i{0}; i < intervals.size(); ++i) {
        for (usize j{i + 1}; j < intervals.size(); ++j) {
            const auto& a{intervals[i]};
            const auto& b{intervals[j]};
            if (a.lo <= b.hi && b.lo <= a.hi) { return b.pat; }
        }
    }
    return stdx::none;
}

} // namespace

// Whether `n` spells a type: an aggregate or type literal, a primitive, or a type name or alias
auto type_resolver::expr_denotes_type(ast::node_id n) -> bool {
    const auto& node{resolving_.ast[n]};
    if (node.is<ast::struct_expr>() || node.is<ast::enum_expr>() || node.is<ast::union_expr>()) {
        return true;
    }
    if (const auto arr{resolving_.ast.get_as_opt<ast::array_expr>(n)}) { return arr->is_type_expr; }
    if (const auto fx{resolving_.ast.get_as_opt<ast::function_expr>(n)}) {
        return fx->is_type_expr;
    }
    if (const auto addr{resolving_.ast.get_as_opt<ast::address_of_expr>(n)}) {
        return expr_denotes_type(*addr->rhs);
    }
    if (const auto ref{resolving_.ast.get_as_opt<ast::reference_expr>(n)}) {
        return expr_denotes_type(*ref->rhs);
    }
    const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(n)};
    if (!ident) { return false; }
    if (syntax::token_type::is_int_type_lexeme(ident->name)) { return true; }
    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
    if (!sym) { return false; }
    if (sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) { return true; }
    const auto nd{sym->get_data().as_opt<symbols::node_t>()};
    if (!nd) { return false; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*nd)};
    if (!decl || !decl->value ||
        !(decl->has_modifier(ast::decl_modifiers::LET) ||
          decl->has_modifier(ast::decl_modifiers::COMPTIME))) {
        return false;
    }
    return expr_denotes_type(**decl->value);
}

// Whether a `match` selects on a compile-time `type`, checking and emitting only the chosen arm
auto type_resolver::is_type_match(const ast::match_expr& match, type& matcher_type) -> bool {
    // `match` on an enum or union type keeps its variant semantics, and empty or incomplete types
    // keep their "cannot match on" diagnostics
    const auto denoted_kind{denoted_type(matcher_type).get_kind()};
    if (denoted_kind == type_kind::ENUM || denoted_kind == type_kind::UNION ||
        (denoted_kind != type_kind::TYPE && !is_value_type(denoted_kind))) {
        return false;
    }
    if (expr_denotes_type(*match.matcher)) { return true; }
    // A `T: type` parameter resolves to its argument inside an instantiation, so a first real
    // arm pattern that is a type also makes a type match
    for (usize i{0}; i < match.arms.size(); ++i) {
        if (match.catch_all_idx && i == *match.catch_all_idx) { continue; }
        if (expr_denotes_type(*match.arms[i].primary_pattern())) { return true; }
        break;
    }
    gir::const_eval type_probe{ctx_, resolving_};
    const auto      cv{type_probe.try_eval(match.matcher)};
    return cv && cv->is<stdx::option<sema::type&>>();
}

// Checks a match over an integer, byte, or bool covers every value, and rejects other primitives
auto type_resolver::validate_scalar_arms(const ast::match_expr& match, type& matcher_type)
    -> stdx::option<diagnostic> {
    const auto at{resolving_.ast.location_of(match.matcher)};
    // It's assumed that any sufficiently large type cannot be fully enumerated
    stdx::option<u16> required_arm_count;
    switch (matcher_type.get_kind()) {
    case type_kind::INT:
        // Only an 8-bit unsigned integer is small enough to enumerate exhaustively
        if (const auto info{as_integer(matcher_type)};
            info && info->bits == 8 && !info->is_signed) {
            required_arm_count.emplace(256);
        }
        break;
    case type_kind::ISIZE:
    case type_kind::USIZE:
    case type_kind::COMPTIME_INT: break;
    case type_kind::BOOL:         required_arm_count.emplace(2); break;
    case type_kind::F16:
    case type_kind::F32:
    case type_kind::F64:
    case type_kind::F80:
    case type_kind::F128:
    case type_kind::COMPTIME_FLOAT:
        return diagnostic{"Cannot match on floats due to precision; use an if statement with "
                          "explicit precision handling",
                          sema::error::TYPE_MISMATCH,
                          at};
    case type_kind::VOID_:
    case type_kind::UNDEFINED:
        return diagnostic{"Empty types cannot be matched on", sema::error::TYPE_MISMATCH, at};
    case type_kind::ENUM_LITERAL:
    case type_kind::TYPE:
    case type_kind::AUTO:
    case type_kind::OPAQUE:
    case type_kind::NORETURN:
    case type_kind::NULLPTR:
        return diagnostic{fmt::format("Can only match on integers, bytes, and booleans; found '{}'",
                                      ctx_.type_display_name(matcher_type)),
                          sema::error::TYPE_MISMATCH,
                          at};
    default: UNREACHABLE("Builtin types should never take this type kind");
    }

    // A range arm can never make an integer or byte match exhaustive on its own
    const bool has_range_arm{std::ranges::any_of(match.arms, [&](const auto& arm) {
        return std::ranges::any_of(arm.patterns, [&](const auto& p) {
            return resolving_.ast.get_as_opt<ast::range_expr>(*p).has_value();
        });
    })};
    if (has_range_arm && matcher_type.get_kind() == type_kind::BOOL) {
        return diagnostic{"Range patterns are not allowed when matching on 'bool'",
                          sema::error::ILLEGAL_MATCH_PATTERN,
                          at};
    }
    if (match.catch_all_idx) { return stdx::none; }
    if (has_range_arm) {
        return diagnostic{"A 'match' with a range pattern requires a catch-all '_' arm",
                          sema::error::ILLEGAL_MATCH_PATTERN,
                          at};
    }
    if (!required_arm_count) {
        return diagnostic{
            fmt::format("Matching on type '{}' requires a catch all arm with a pattern of '_'",
                        ctx_.type_display_name(matcher_type)),
            sema::error::TYPE_MISMATCH,
            at};
    }
    usize listed_pattern_count{0};
    for (const auto& arm : match.arms) { listed_pattern_count += arm.patterns.size(); }
    if (*required_arm_count == listed_pattern_count) { return stdx::none; }
    return diagnostic{fmt::format("Matching on type '{}' requires a catch all arm with a pattern "
                                  "of '_' or exactly {} patterned arms",
                                  ctx_.type_display_name(matcher_type),
                                  *required_arm_count),
                      sema::error::TYPE_MISMATCH,
                      at};
}

// The type an arm's capture binds: the matcher itself, or the payload every listed union
// variant shares. `none` when the variants carry different payloads
auto type_resolver::arm_capture_base(const ast::match_expr::arm& arm, type& matcher_type)
    -> stdx::option<type&> {
    const auto union_data{matcher_type.get_data().as_opt<types::union_t>()};
    // A catch-all arm captures the whole scrutinee, whichever variant it holds
    if (!union_data || arm.primary_pattern().is<ast::discarded>()) { return matcher_type; }
    const auto&         table{ctx_.registry.get(matcher_type.get_symbol_table_idx())};
    stdx::option<type&> base_type;
    for (const auto& pat : arm.patterns) {
        const auto ia{resolving_.ast.get_as_opt<ast::implicit_access_expr>(*pat)};
        ASSERT(ia, "Union validator failed to error");
        const auto& pident{resolving_.ast.get_as<ast::identifier_expr>(ia->member)};
        auto&       pty{union_data->type_at(table.get_proxy(pident.name).index)};
        if (!base_type) {
            base_type.emplace(pty);
        } else if (!sema::is_same_unqualified(*base_type, pty)) {
            return stdx::none;
        }
    }
    return base_type;
}

// Why a scalar pattern `value` can't match `matcher_type`, if it can't
auto type_resolver::scalar_pattern_mismatch(ast::node_id     value,
                                            type&            matcher_type,
                                            gir::const_eval& probe) -> stdx::option<std::string> {
    const auto vt{resolving_.get_sema_type_opt(value)};
    if (!vt || vt->is_poison() || is_generic_type(*vt, false) ||
        is_generic_type(matcher_type, false)) {
        return stdx::none;
    }
    if (!is_assignable(*vt, matcher_type)) {
        return fmt::format("A pattern of type '{}' cannot match a value of type '{}'",
                           ctx_.type_display_name(*vt),
                           ctx_.type_display_name(matcher_type));
    }
    const auto folded{probe.try_eval(value)};
    const auto as_int{folded ? folded->as_int_opt() : stdx::none};
    if (as_int && is_integer(matcher_type.get_kind()) &&
        !comptime_int_fits(*as_int, matcher_type, target_ptr_bits())) {
        return fmt::format(
            "Pattern {} is out of range for '{}'", *as_int, ctx_.type_display_name(matcher_type));
    }
    return stdx::none;
}

auto type_resolver::visit(ast::node_id id, const ast::match_expr& match) -> void {
    PROFILE_FUNCTION();
    {
        // The value being matched has its own type, not the one the `match` result flows into
        const structural_guard shield{implicit_type_stack_, nullptr};
        TRY_RESOLVE(match.matcher);
    }
    auto& matcher_type{*last_type_.take()};

    // See `visit(if_expr)`: skip a `match comptime` in the template pass so a config-specific
    // dead arm never poisons; `resolve_param_impl_bodies` selects the arm per instantiation.
    if (match.is_comptime && building_param_template_) {
        auto& void_t{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        resolving_.set_sema_type(id, void_t);
        return last_type_.emplace(void_t);
    }

    // `match comptime` folds its scrutinee and type-checks only the arm it selects.
    if (match.is_comptime) { return resolve_comptime_match(id, match, matcher_type); }

    // A scrutinee that denotes a compile-time `type` takes the type-match path: only the
    // selected arm is checked/emitted, `if comptime`-style.
    if (is_type_match(match, matcher_type)) { return resolve_type_match(id, match, matcher_type); }

    // The expression must resolve to a single type on pass 3
    stdx::option<type&> effective_matcher_type{matcher_type};
    if (const auto ref{matcher_type.get_data().as_opt<types::reference>()}) {
        effective_matcher_type.emplace(ref->underlying);
    }

    stdx::option<type&>       first_type;
    std::vector<result_value> arm_values;
    // A block arm yields nothing, so a `match` statement with one has no value to keep
    bool                           block_arm{false};
    stdx::option<ast::expr_handle> type_arm;
    bool                           value_arm{false};
    bool                           matcher_is_const{false};
    bool                           matcher_is_addressable{false};
    if (matcher_type.get_data().is<types::reference>()) {
        matcher_is_const       = matcher_type.is_constant();
        matcher_is_addressable = true;
    } else {
        if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(match.matcher)}) {
            if (const auto sym{ctx_.registry.lookup(table_stack_, ident->name)}) {
                if (const auto node{sym->get_data().as_opt<symbols::node_t>()}) {
                    if (const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)}) {
                        matcher_is_const = decl->has_modifier(ast::decl_modifiers::LET) ||
                                           decl->has_modifier(ast::decl_modifiers::COMPTIME);
                    }
                }
            }
        }
        matcher_is_addressable = is_lvalue_shape(resolving_, match.matcher);
    }

    // Rip through the arms once to validate structural arm rules
    const auto& matcher_data{effective_matcher_type->get_data()};
    if (matcher_data.is<types::enum_t>()) {
        if (auto diag{validate_enum_arms(id, match, *effective_matcher_type)}; diag) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(diag).value()));
        }
    } else if (matcher_data.is<types::union_t>()) {
        if (auto diag{validate_union_arms(id, match, *effective_matcher_type)}; diag) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(diag).value()));
        }
    } else if (matcher_data.is<types::builtin_type>() || matcher_data.is<types::integer>()) {
        if (auto diag{validate_scalar_arms(match, *effective_matcher_type)}) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(diag).value()));
        }
    } else {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("Can only match on enums, unions, and certain primitive types; found '{}'",
                        ctx_.type_display_name(*effective_matcher_type)),
            sema::error::TYPE_MISMATCH,
            resolving_.ast.location_of(match.matcher)));
    }

    std::vector<arm_interval> const_intervals;
    gir::const_eval           interval_probe{ctx_, resolving_};
    const bool                scalar_match{
        (matcher_data.is<types::builtin_type>() || matcher_data.is<types::integer>()) &&
        effective_matcher_type->get_kind() != type_kind::BOOL};

    // Each arm was assigned a new scope index on the first pass
    for (const auto& arm : match.arms) {
        // Tabled types have prefilled types that should be pushed on the table stack
        auto&       arm_table_type{resolving_.get_sema_type(arm)};
        const scope scope{table_stack_, arm_table_type.get_symbol_table_idx(), table_idx_};

        if (arm.capture && arm.capture->payload.is<ast::identifier_expr>()) {
            const auto base_type{arm_capture_base(arm, *effective_matcher_type)};
            if (!base_type) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "A capture on a multi-variant match arm requires every listed variant to "
                    "carry the same payload type",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving_.ast.location_of(*arm.capture->payload)));
            }

            auto cap_result{
                resolve_capture_modifier(ctx_,
                                         arm.capture->modifier,
                                         *base_type,
                                         matcher_is_const,
                                         matcher_is_addressable,
                                         "value",
                                         resolving_.ast.location_of(*arm.capture->payload),
                                         place_mutability(match.matcher) == types::mut::POLY)};
            if (!cap_result) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_, id, std::move(cap_result).error()));
            }
            resolving_.set_sema_type(*arm.capture->payload, **cap_result);
            resolve_symbol_info(arm.capture->payload, symbol_kind::VALUE);
        }

        // Each pattern is resolved against the matcher's type
        for (const auto& pattern : arm.patterns) {
            if (pattern.is<ast::discarded>()) { continue; }

            const auto range{resolving_.ast.get_as_opt<ast::range_expr>(*pattern)};
            if (range && !scalar_match) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "Range patterns are only valid when matching on an integer or byte value",
                    error::ILLEGAL_MATCH_PATTERN,
                    resolving_.ast.location_of(*pattern)));
            }

            // `.a .b` (a missing comma) parses as a member of the variant `.a`
            if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(*pattern)};
                dot && resolving_.ast.get_as_opt<ast::implicit_access_expr>(dot->object)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A variant pattern has no members; separate patterns with a "
                                     "comma (`.a, .b`)",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*pattern)));
            }

            if (range && (!range->lhs || !range->rhs)) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A range pattern needs both endpoints; open-ended ranges are "
                                     "only valid inside a subscript",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*pattern)));
            }

            {
                const structural_guard pattern_g{implicit_type_stack_, *effective_matcher_type};
                if (range) {
                    TRY_RESOLVE(*range->lhs);
                    TRY_RESOLVE(*range->rhs);
                } else {
                    TRY_RESOLVE(pattern);
                }
            }

            // Matching a value compares it, which a type has nothing to compare with
            if (!range && operand_nature(ast::expr_handle{*pattern}) == operand_nature_t::TYPE) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "A `match` on a value expects its patterns to be values, but "
                                     "this one is a type",
                                     error::ILLEGAL_MATCH_PATTERN,
                                     resolving_.ast.location_of(*pattern)));
            }

            // A scalar pattern must be a value the matcher's type can actually hold
            if (scalar_match || effective_matcher_type->get_kind() == type_kind::BOOL) {
                const auto values{range ? std::vector<ast::node_id>{*range->lhs, *range->rhs}
                                        : std::vector<ast::node_id>{*pattern}};
                for (const auto value : values) {
                    if (const auto msg{scalar_pattern_mismatch(
                            value, *effective_matcher_type, interval_probe)}) {
                        return last_type_.emplace(
                            ctx_.poison_node(resolving_,
                                             id,
                                             *msg,
                                             error::ILLEGAL_MATCH_PATTERN,
                                             resolving_.ast.location_of(value)));
                    }
                }
            }

            if (!scalar_match) { continue; }
            if (range) {
                const bool inclusive{(*pattern).get_token_type() ==
                                     syntax::token_type_t::DOT_DOT_EQ};
                const auto lo{interval_probe.try_eval(*range->lhs)};
                const auto hi{interval_probe.try_eval(*range->rhs)};
                if (!lo || !hi) { continue; }
                const auto lo_i{lo->as_int_opt()};
                const auto hi_i{hi->as_int_opt()};
                if (!lo_i || !hi_i) { continue; }
                const auto lo_closed{static_cast<i64>(*lo_i)};
                const i64  hi_closed{static_cast<i64>(inclusive ? *hi_i : *hi_i - 1)};
                if (hi_closed < lo_closed) {
                    return last_type_.emplace(ctx_.poison_node(
                        resolving_,
                        id,
                        "Range pattern is empty; its lower bound exceeds its upper bound",
                        error::ILLEGAL_MATCH_PATTERN,
                        resolving_.ast.location_of(*pattern)));
                }
                const_intervals.emplace_back(lo_closed, hi_closed, pattern);
            } else if (const auto pv{interval_probe.try_eval(*pattern)}) {
                if (const auto v{pv->as_int_opt()}) {
                    const_intervals.emplace_back(
                        static_cast<i64>(*v), static_cast<i64>(*v), pattern);
                }
            }
        }
        {
            const mutating_context_guard branch_g{in_expr_branch_, true};
            const mutating_context_guard unused_g{arm_of_unused_,
                                                  unused_value_nodes_.contains(id.get_index())};
            const scope_guard arm_g{enclosing_arms_,
                                    enclosing_arm{
                                        .match   = match,
                                        .arm_idx = static_cast<usize>(&arm - match.arms.data()),
                                    }};
            TRY_RESOLVE(arm.dispatch);
        }

        // Only an expr_stmt arm can yield a value (blocks never do, per emit_stmt_as_value). A
        // block's own resolved type is just its scope handle, not a value type, so it's ignored.
        if (resolving_.ast.get_as_opt<ast::block_stmt>(arm.dispatch)) { block_arm = true; }
        if (const auto expr_stmt_node{resolving_.ast.get_as_opt<ast::expr_stmt>(arm.dispatch)}) {
            if (const auto inner_type{resolving_.get_sema_type_opt(expr_stmt_node->expression)}) {
                if (!inner_type->is_poison()) {
                    // Never let a noreturn arm win (#252)
                    if (inner_type->get_kind() != type_kind::NORETURN && !first_type) {
                        first_type = *inner_type;
                    }
                    arm_values.emplace_back(inner_type.get(), *expr_stmt_node->expression);
                    if (decl_value_denotes_type(expr_stmt_node->expression)) {
                        if (!type_arm) { type_arm.emplace(expr_stmt_node->expression); }
                    } else if (inner_type->get_kind() != type_kind::NORETURN) {
                        value_arm = true;
                    }
                }
            }
        }
    }

    // Arms that yield a type next to arms that yield a value have no common result
    if (type_arm && value_arm) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            *type_arm,
            fmt::format("This match arm yields the type '{}' where the other arms yield values",
                        ctx_.type_display_name(denoted_type(resolving_.get_sema_type(*type_arm)))),
            error::TYPE_USED_AS_VALUE,
            resolving_.ast.location_of(*type_arm)));
    }

    // Two arm patterns that both fold to constants may not cover the same value
    if (const auto overlap{first_overlapping_pattern(const_intervals)}) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "This match arm pattern overlaps an earlier arm",
                                                   error::ILLEGAL_MATCH_PATTERN,
                                                   resolving_.ast.location_of(*overlap)));
    }

    if (!first_type || (block_arm && unused_value_nodes_.contains(id.get_index()))) {
        first_type.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
        arm_values.clear();
    }
    auto& result_type{arm_values.size() > 1 ? result_peer(id, arm_values) : *first_type};
    resolving_.set_sema_type(id, result_type);
    last_type_.emplace(result_type);
}

auto type_resolver::arm_denotes_type(ast::node_id at) const -> bool {
    // Only forms that can name a type are worth folding to find out
    const bool can_name_type{[&] {
        switch (at.get_kind()) {
        case ast::node_kind::IDENTIFIER_EXPRESSION:
        case ast::node_kind::DOT_EXPRESSION:
        case ast::node_kind::TYPE_EXPRESSION:
        case ast::node_kind::STRUCT_EXPRESSION:
        case ast::node_kind::UNION_EXPRESSION:
        case ast::node_kind::ENUM_EXPRESSION:
        case ast::node_kind::INTERFACE_EXPRESSION:
        case ast::node_kind::ADDRESS_OF_EXPRESSION:
        case ast::node_kind::REFERENCE_EXPRESSION:  return true;
        case ast::node_kind::ARRAY_EXPRESSION:
            return resolving_.ast.get_as<ast::array_expr>(at).is_type_expr;
        case ast::node_kind::FUNCTION_EXPRESSION:
            return resolving_.ast.get_as<ast::function_expr>(at).is_type_expr;
        case ast::node_kind::IF_EXPRESSION:
        case ast::node_kind::MATCH_EXPRESSION:
        case ast::node_kind::LABEL_EXPRESSION:
        case ast::node_kind::CALL_EXPRESSION:
        case ast::node_kind::INDEX_EXPRESSION:
        case ast::node_kind::COMPTIME_EXPRESSION: {
            if (type_valued_results_.contains(at.get_index())) { return true; }
            const auto t{resolving_.get_sema_type_opt(at)};
            return t && t->get_kind() == type_kind::TYPE;
        }
        default: return false;
        }
    }()};
    return can_name_type && operand_nature(ast::expr_handle{at}) == operand_nature_t::TYPE;
}

// The type the values of an `if`, a `match`, or a labeled block meet at. A `void` value among
// them means the construct is a statement, where the first value's type stands.
auto type_resolver::result_peer(ast::node_id id, gsl::span<const result_value> values) -> type& {
    std::vector<peer_operand> operands;
    stdx::option<type&>       first;
    for (const auto& value : values) {
        const auto kind{value.type->get_kind()};
        if (kind != type_kind::NORETURN && !first) { first.emplace(*value.type); }
        operands.emplace_back(&peer_view(*value.type));
    }
    if (!first) { return *values.front().type; }

    // Arms that all spell types (`if (c) u8 else S`) make it a type value, typed like a name for
    // the type it folds to
    if (std::ranges::all_of(values, [&](const result_value& value) {
            return value.type->get_kind() == type_kind::NORETURN || arm_denotes_type(value.at);
        })) {
        type_valued_results_.insert(id.get_index());
        const auto folded{probe_fold(ast::expr_handle{id})};
        const auto instance{folded ? folded->as_opt<stdx::option<type&>>() : stdx::none};
        return instance && *instance ? **instance : ctx_.get_builtin_resolved_type(type_kind::TYPE);
    }

    // Only values can peer. Types and scope handles keep the first arm's type
    const bool all_values{std::ranges::all_of(operands, [](const peer_operand& value) {
        const auto kind{value.type->get_kind()};
        const bool takes_no_part{kind == type_kind::NORETURN || kind == type_kind::UNDEFINED ||
                                 kind == type_kind::POISON};
        return takes_no_part || (kind != type_kind::TYPE && is_value_type(kind));
    })};
    if (!all_values) { return *first; }

    const auto peer{peer_type(ctx_, operands)};
    if (peer) { return **peer; }
    // Arms nothing reads need no common type. The construct is a statement
    if (unused_value_nodes_.contains(id.get_index())) {
        return ctx_.get_builtin_resolved_type(type_kind::VOID_);
    }
    return ctx_.poison_node(resolving_,
                            id,
                            peer_error_message(ctx_, operands, peer.error()),
                            error::NO_PEER_TYPE,
                            resolving_.ast.location_of(values[peer.error().second].at));
}

auto type_resolver::visit(ast::node_id id, const ast::while_loop_expr& while_loop) -> void {
    PROFILE_FUNCTION();
    // Unrolling itself is an emit-time concern, the resolver only needs to type-check the
    // condition/body once
    if (while_loop.is_unrolled) { check_unrolled_loop_jumps(while_loop.block); }
    TRY_RESOLVE(while_loop.condition);
    if (reject_type_condition(while_loop.condition)) { return; }
    // The loop itself holds the block index which houses captures, not the block
    auto& loop_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!loop_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    if (const auto poisoned{resolve_unwrap_captures(id,
                                                    while_loop.condition,
                                                    while_loop.payload_capture,
                                                    while_loop.else_capture,
                                                    loop_type.get_symbol_table_idx(),
                                                    "while",
                                                    while_loop.is_unrolled)}) {
        return last_type_.emplace(*poisoned);
    }
    {
        const scope s{table_stack_, loop_type.get_symbol_table_idx(), table_idx_};
        // The continuation shares the loop's scope, where a payload capture lives
        if (while_loop.continuation) {
            resolve(*while_loop.continuation);
            if (!last_type_ || last_type_->is_poison()) { return fail_scoped_body(); }
        }
        const auto& block{resolving_.ast.get_as<ast::block_stmt>(while_loop.block)};
        const mutating_context_guard cx_loop_g{in_unrolled_loop_,
                                               while_loop.is_unrolled || in_unrolled_loop_};
        // A body that changes a `comptime let mut` type is typed once per iteration
        const bool per_iteration{while_loop.is_unrolled && !while_loop.continuation &&
                                 assigns_comptime_type_var(*while_loop.block)};
        if (per_iteration ? resolve_unrolled_while_iterations(id, while_loop, block)
                          : resolve_block_statements(block)) {
            return fail_scoped_body();
        }
    }

    if (while_loop.non_break) {
        stdx::option<scope> capture_s;
        if (const auto idx{capture_scope(while_loop.else_capture)}) {
            capture_s.emplace(table_stack_, *idx, table_idx_);
        }
        const mutating_context_guard branch_g{in_expr_branch_, true};
        const mutating_context_guard unused_g{arm_of_unused_,
                                              unused_value_nodes_.contains(id.get_index())};
        TRY_RESOLVE(*while_loop.non_break);
    }
    resolving_.set_sema_type(
        id, loop_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : loop_type);
    last_type_.emplace(resolving_.get_sema_type(id));
}

auto type_resolver::assigns_comptime_type_var(ast::node_id root) -> bool {
    bool       found{false};
    const auto walk{[&](this const auto& self, ast::node_id n) -> void {
        if (found) { return; }
        resolving_.ast[n].visit(
            [&](const ast::assignment_expr& data) {
                if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(data.lhs)}) {
                    const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
                    if (sym && sym->has_kind() && sym->get_kind() == symbol_kind::TYPE) {
                        found = true;
                    }
                }
            },
            [&](const ast::block_stmt& data) {
                for (const auto s : data) { self(*s); }
            },
            [&](const ast::expr_stmt& data) { self(*data.expression); },
            [&](const ast::if_expr& data) {
                self(*data.consequence);
                if (data.alternate) { self(*data.alternate); }
            },
            [&](const ast::label_expr& data) { self(*data.body); },
            [&](const auto&) {});
    }};
    walk(root);
    return found;
}

auto type_resolver::resolve_unrolled_while_iterations(ast::node_id                id,
                                                      const ast::while_loop_expr& while_loop,
                                                      const ast::block_stmt&      block) -> bool {
    const auto saved_for_gi{for_generic_instantiation_};
    const auto saved_floor{reresolve_floor_};
    for_generic_instantiation_ = true;
    reresolve_floor_.emplace(resolving_.get_sema_type(id).get_symbol_table_idx());
    const auto restore_reresolve{gsl::finally([&] {
        for_generic_instantiation_ = saved_for_gi;
        reresolve_floor_           = saved_floor;
    })};

    stdx::option<comptime_frame> carried;
    constexpr usize              max_iterations{1'024};
    for (usize k{0}; k < max_iterations; ++k) {
        // The condition sees what the previous iteration left behind
        stdx::option<comptime_frame> start;
        {
            const active_block_guard guard{active_blocks_, block};
            active_blocks_.back().carried = carried;
            start.emplace(make_simulated_frame());
        }
        stdx::option<bool> keep_going;
        {
            const comptime_frame_guard cfg{ctx_.comptime_binding_frames, std::move(*start)};
            gir::const_eval            evaluator{ctx_, resolving_};
            const gir::const_eval::comptime_context_guard g{evaluator, true};
            const auto cond{evaluator.try_eval(while_loop.condition)};
            if (const auto folded{cond ? cond->as_opt<bool>() : stdx::none}) {
                keep_going = *folded;
            }
        }
        // An unfoldable condition is reported when the loop is unrolled
        if (!keep_going || !*keep_going) { return false; }

        ctx_.advance_epoch();
        const body_typing_snapshot snap{resolving_};
        const iteration_write_log  writes{resolving_};
        const active_block_guard   guard{active_blocks_, block};
        active_blocks_.back().carried = carried;
        for (usize idx{0}; idx < block.statements.size(); ++idx) {
            active_blocks_.back().current_stmt_idx = idx;
            resolve(block.statements[idx]);
            if (last_type_->is_poison()) { return true; }
        }
        active_blocks_.back().current_stmt_idx = block.statements.size();
        carried.emplace(make_simulated_frame());

        body_type_diff typing;
        snap.diff_iteration_into(ctx_, resolving_, typing, writes.get());
        ctx_.instantiation_cache.set_body_type_diff(
            fmt::format("{}whileloop#{}#{}",
                        typing_scope_prefix_.empty() ? std::string{} : typing_scope_prefix_ + "#",
                        id.get_index(),
                        k),
            std::move(typing));
    }
    return false;
}

// DONT CALL ME FROM ANY LOOP/CONDITION/FN RESOLVER
auto type_resolver::visit(ast::node_id id, const ast::block_stmt& block) -> void {
    PROFILE_FUNCTION();
    // Only blocks in statement or branch position get a scope (`{ ... }[1]` never does)
    if (!resolving_.has_sema_type(id)) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "a block can't be used as a value here",
                                                   error::TYPE_MISMATCH,
                                                   resolving_.ast.location_of(id)));
    }
    auto& block_type{resolving_.get_sema_type(id)};
    // Poisoned by an earlier error (e.g. a prior unrolling). Its scope is gone
    if (!block_type.has_symbol_table_idx()) { return last_type_.emplace(ctx_.get_poison()); }
    const scope s{table_stack_, block_type.get_symbol_table_idx(), table_idx_};

    // A block that is an `if`/`match` branch never yields a value
    in_expr_branch_ = false;
    const comptime_evaluation_scope cx_scope{ctx_, block.is_comptime};

    // Just an abridged loop handler
    if (resolve_block_statements(block)) { return fail_scoped_body(); }
    resolving_.set_sema_type(
        id, block_type.is_poison() ? ctx_.get_builtin_resolved_type(type_kind::VOID_) : block_type);
    last_type_.emplace(resolving_.get_sema_type(id));
}

// Materializes the current compile-time values of mutable `comptime let mut` locals
// by simulating preceding statements in the active lexical block hierarchy.
auto type_resolver::make_simulated_frame() -> comptime_frame {
    comptime_frame frame;
    if (active_blocks_.empty()) { return frame; }
    gir::const_eval evaluator{ctx_, resolving_};
    evaluator.simulate_active_blocks(active_blocks_, frame);
    return frame;
}

auto type_resolver::resolve_control_flow_label(stdx::option<ast::identifier_handle> label,
                                               std::string_view                     stmt_name)
    -> stdx::result<stdx::option<symbol&>, diagnostic> {
    if (label) {
        const auto& ident{resolving_.ast.get_as<ast::identifier_expr>(*label)};
        auto        sym{ctx_.registry.lookup(table_stack_, ident.name)};
        if (!sym || sym->get_kind() != symbol_kind::LABEL) {
            return make_sema_err(
                fmt::format("Labeled {} statements must be used with a known label", stmt_name),
                error::ILLEGAL_CONTROL_FLOW,
                resolving_.ast.location_of(*label));
        }
        return sym;
    }
    return stdx::none;
}

auto type_resolver::visit(ast::node_id id, const ast::break_stmt& break_stmt) -> void {
    PROFILE_FUNCTION();
    auto result{resolve_control_flow_label(break_stmt.label, "break")};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    // No payload is semantically equivalent to breaking with void
    if (auto sym{result.value()}) {
        auto& label_data{symbols::label::from(*sym)};

        if (break_stmt.expression) {
            TRY_RESOLVE(*break_stmt.expression);
            auto& expression_type{*last_type_.take()};
            label_data.add_yield_type(expression_type);
            resolving_.set_sema_type(id, expression_type);
        } else {
            auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
            label_data.add_yield_type(void_type);
            resolving_.set_sema_type(id, void_type);
        }
    } else {
        resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID_));
    }

    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::visit(ast::node_id id, const ast::continue_stmt& continue_stmt) -> void {
    PROFILE_FUNCTION();
    auto result{resolve_control_flow_label(continue_stmt.label, "continue")};
    if (!result) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(result).error()));
    }

    resolving_.set_sema_type(id, ctx_.get_builtin_resolved_type(type_kind::VOID_));
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

auto type_resolver::check_deferred_body_jumps(ast::stmt_handle body) -> void {
    ankerl::unordered_dense::set<std::string_view> local_labels;

    auto collect_labels = [&](auto& self, ast::node_id n) -> void {
        resolving_.ast[n].visit(
            [&](const ast::label_expr& data) {
                if (data.name) {
                    local_labels.emplace(
                        resolving_.ast.get_as<ast::identifier_expr>(*data.name).name);
                }
                self(self, *data.body);
            },
            [&](const ast::while_loop_expr& data) { self(self, *data.block); },
            [&](const ast::for_loop_expr& data) { self(self, *data.block); },
            [&](const ast::infinite_loop_expr& data) { self(self, *data.block); },
            [&](const ast::do_while_loop_expr& data) { self(self, *data.block); },
            [&](const ast::block_stmt& data) {
                for (const auto s : data) { self(self, *s); }
            },
            [&](const ast::expr_stmt& data) { self(self, *data.expression); },
            [&](const ast::if_expr& data) {
                self(self, *data.consequence);
                if (data.alternate) { self(self, *data.alternate); }
            },
            [&](const ast::match_expr& data) {
                for (const auto& arm : data.arms) { self(self, *arm.dispatch); }
            },
            [&](const ast::defer_stmt& data) { self(self, *data.deferred); },
            [&](const ast::errdefer_stmt& data) { self(self, *data.deferred); },
            [&](const auto&) { return; });
    };
    collect_labels(collect_labels, *body);

    auto check_jumps = [&](auto& self, ast::node_id n, usize loop_depth) -> void {
        resolving_.ast[n].visit(
            [&](const ast::return_stmt&) {
                ctx_.diags.emplace_back("cannot 'return' from inside a 'defer' body",
                                        error::DEFER_BODY_JUMP,
                                        resolving_.ast.location_of(n));
            },
            [&](const ast::unwrap_expr& data) {
                if (n.get_token_type() == syntax::token_type_t::QUESTION) {
                    ctx_.diags.emplace_back("cannot use '?' operator inside a 'defer' body",
                                            error::DEFER_BODY_JUMP,
                                            resolving_.ast.location_of(n));
                }
                self(self, *data.operand, loop_depth);
            },
            [&](const ast::break_stmt& data) {
                bool illegal{false};
                if (data.label) {
                    const auto& name{resolving_.ast.get_as<ast::identifier_expr>(*data.label).name};
                    if (!local_labels.contains(name)) { illegal = true; }
                } else if (loop_depth == 0) {
                    illegal = true;
                }
                if (illegal) {
                    ctx_.diags.emplace_back("cannot 'break' from inside a 'defer' body",
                                            error::DEFER_BODY_JUMP,
                                            resolving_.ast.location_of(n));
                }
                if (data.expression) { self(self, **data.expression, loop_depth); }
            },
            [&](const ast::continue_stmt& data) {
                bool illegal{false};
                if (data.label) {
                    const auto& name{resolving_.ast.get_as<ast::identifier_expr>(*data.label).name};
                    if (!local_labels.contains(name)) { illegal = true; }
                } else if (loop_depth == 0) {
                    illegal = true;
                }
                if (illegal) {
                    ctx_.diags.emplace_back("cannot 'continue' from inside a 'defer' body",
                                            error::DEFER_BODY_JUMP,
                                            resolving_.ast.location_of(n));
                }
            },
            [&](const ast::while_loop_expr& data) {
                self(self, *data.condition, loop_depth);
                if (data.continuation) { self(self, **data.continuation, loop_depth); }
                self(self, *data.block, loop_depth + 1);
                if (data.non_break) { self(self, **data.non_break, loop_depth); }
            },
            [&](const ast::for_loop_expr& data) {
                for (const auto it : data.iterables) { self(self, *it, loop_depth); }
                self(self, *data.block, loop_depth + 1);
                if (data.non_break) { self(self, **data.non_break, loop_depth); }
            },
            [&](const ast::infinite_loop_expr& data) { self(self, *data.block, loop_depth + 1); },
            [&](const ast::do_while_loop_expr& data) {
                self(self, *data.condition, loop_depth);
                self(self, *data.block, loop_depth + 1);
            },
            [&](const ast::block_stmt& data) {
                for (const auto s : data) { self(self, *s, loop_depth); }
            },
            [&](const ast::expr_stmt& data) { self(self, *data.expression, loop_depth); },
            [&](const ast::discard_stmt& data) { self(self, *data.discarded, loop_depth); },
            [&](const ast::defer_stmt& data) { self(self, *data.deferred, loop_depth); },
            [&](const ast::errdefer_stmt& data) { self(self, *data.deferred, loop_depth); },
            [&](const ast::decl_stmt& data) {
                if (data.value) { self(self, **data.value, loop_depth); }
            },
            [&](const ast::if_expr& data) {
                // `if comptime { ... }` has no condition
                if (data.condition) { self(self, **data.condition, loop_depth); }
                self(self, *data.consequence, loop_depth);
                if (data.alternate) { self(self, *data.alternate, loop_depth); }
            },
            [&](const ast::match_expr& data) {
                self(self, *data.matcher, loop_depth);
                for (const auto& arm : data.arms) { self(self, *arm.dispatch, loop_depth); }
            },
            [&](const ast::binary_expr& data) {
                self(self, *data.lhs, loop_depth);
                self(self, *data.rhs, loop_depth);
            },
            [&](const ast::assignment_expr& data) {
                self(self, *data.lhs, loop_depth);
                self(self, *data.rhs, loop_depth);
            },
            [&](const ast::unary_expr& data) { self(self, *data.rhs, loop_depth); },
            [&](const ast::reference_expr& data) { self(self, *data.rhs, loop_depth); },
            [&](const ast::dereference_expr& data) { self(self, *data.rhs, loop_depth); },
            [&](const ast::comptime_expr& data) { self(self, *data.rhs, loop_depth); },
            [&](const ast::address_of_expr& data) { self(self, *data.rhs, loop_depth); },
            [&](const ast::call_expr& data) {
                self(self, *data.function, loop_depth);
                for (const auto& arg : data.arguments) {
                    if (const auto eh = arg.template as_opt<ast::expr_handle>()) {
                        self(self, **eh, loop_depth);
                    }
                }
            },
            [&](const ast::dot_expr& data) { self(self, *data.object, loop_depth); },
            [&](const ast::index_expr& data) {
                self(self, *data.array, loop_depth);
                self(self, *data.index, loop_depth);
            },
            [&](const ast::range_expr& data) {
                if (data.lhs) { self(self, **data.lhs, loop_depth); }
                if (data.rhs) { self(self, **data.rhs, loop_depth); }
            },
            [&](const ast::array_expr& data) {
                for (const auto item : data.items) { self(self, *item, loop_depth); }
            },

            [&](const ast::label_expr& data) { self(self, *data.body, loop_depth); },
            [&](const ast::initializer_expr& data) {
                for (const auto& init : data.initializers) { self(self, *init.value, loop_depth); }
            },
            [&](const ast::cfg_stmt& data) {
                for (const auto& arm : data.arms) {
                    if (arm.predicate) { self(self, **arm.predicate, loop_depth); }
                    for (const auto it : arm.items) { self(self, *it, loop_depth); }
                }
            },
            [&](const ast::cfg_value_expr& data) {
                if (data.predicate) { self(self, **data.predicate, loop_depth); }
                for (const auto& guard : data.guards) {
                    for (const auto predicate : guard.predicates) {
                        self(self, *predicate, loop_depth);
                    }
                    self(self, *guard.value, loop_depth);
                }
                if (data.fallback) { self(self, **data.fallback, loop_depth); }
            },
            [&](const auto&) {});
    };
    check_jumps(check_jumps, *body, 0);
}

auto type_resolver::check_unrolled_loop_jumps(ast::stmt_handle body) -> void {
    ankerl::unordered_dense::set<std::string_view> local_labels;

    auto collect_labels = [&](auto& self, ast::node_id n) -> void {
        resolving_.ast[n].visit(
            [&](const ast::label_expr& data) {
                if (data.name) {
                    local_labels.emplace(
                        resolving_.ast.get_as<ast::identifier_expr>(*data.name).name);
                }
                self(self, *data.body);
            },
            [&](const ast::while_loop_expr& data) { self(self, *data.block); },
            [&](const ast::for_loop_expr& data) { self(self, *data.block); },
            [&](const ast::infinite_loop_expr& data) { self(self, *data.block); },
            [&](const ast::do_while_loop_expr& data) { self(self, *data.block); },
            [&](const ast::block_stmt& data) {
                for (const auto s : data) { self(self, *s); }
            },
            [&](const ast::expr_stmt& data) { self(self, *data.expression); },
            [&](const ast::if_expr& data) {
                self(self, *data.consequence);
                if (data.alternate) { self(self, *data.alternate); }
            },
            [&](const ast::match_expr& data) {
                for (const auto& arm : data.arms) { self(self, *arm.dispatch); }
            },
            [&](const auto&) { return; });
    };
    collect_labels(collect_labels, *body);

    // `loop_depth` counts nested ordinary loops declared inside this unrolled loop's own body
    // `runtime_control_flow_depth` tracks nesting inside runtime branches (if/match)
    auto check_jumps = [&](auto&        self,
                           ast::node_id n,
                           usize        loop_depth,
                           usize        runtime_control_flow_depth) -> void {
        resolving_.ast[n].visit(
            [&](const ast::break_stmt& data) {
                const bool unlabeled_and_local{!data.label && loop_depth == 0};
                const bool labeled_outward{
                    data.label &&
                    !local_labels.contains(
                        resolving_.ast.get_as<ast::identifier_expr>(*data.label).name)};
                if (labeled_outward) {
                    ctx_.diags.emplace_back("'break' cannot target a loop outside an unrolled loop",
                                            error::UNROLLED_LOOP_BREAK,
                                            resolving_.ast.location_of(n));
                } else if (unlabeled_and_local) {
                    if (runtime_control_flow_depth > 0) {
                        ctx_.diags.emplace_back(
                            "'break' inside an unrolled loop is only allowed within compile-time "
                            "control flow",
                            error::UNROLLED_LOOP_BREAK,
                            resolving_.ast.location_of(n));
                    }
                }
                if (data.expression) {
                    self(self, **data.expression, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::continue_stmt& data) {
                const bool unlabeled_and_local{!data.label && loop_depth == 0};
                const bool labeled_outward{
                    data.label &&
                    !local_labels.contains(
                        resolving_.ast.get_as<ast::identifier_expr>(*data.label).name)};
                if (labeled_outward) {
                    ctx_.diags.emplace_back(
                        "'continue' cannot target a loop outside an unrolled loop",
                        error::UNROLLED_LOOP_CONTINUE,
                        resolving_.ast.location_of(n));
                } else if (unlabeled_and_local) {
                    if (runtime_control_flow_depth > 0) {
                        ctx_.diags.emplace_back(
                            "'continue' inside an unrolled loop is only allowed within "
                            "compile-time control flow (such as `if comptime`)",
                            error::UNROLLED_LOOP_CONTINUE,
                            resolving_.ast.location_of(n));
                    }
                }
            },
            [&](const ast::while_loop_expr& data) {
                self(self, *data.condition, loop_depth, runtime_control_flow_depth);
                if (data.continuation) {
                    self(self, **data.continuation, loop_depth, runtime_control_flow_depth);
                }
                self(self, *data.block, loop_depth + 1, runtime_control_flow_depth);
                if (data.non_break) {
                    self(self, **data.non_break, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::for_loop_expr& data) {
                for (const auto it : data.iterables) {
                    self(self, *it, loop_depth, runtime_control_flow_depth);
                }
                self(self, *data.block, loop_depth + 1, runtime_control_flow_depth);
                if (data.non_break) {
                    self(self, **data.non_break, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::infinite_loop_expr& data) {
                self(self, *data.block, loop_depth + 1, runtime_control_flow_depth);
            },
            [&](const ast::do_while_loop_expr& data) {
                self(self, *data.condition, loop_depth, runtime_control_flow_depth);
                self(self, *data.block, loop_depth + 1, runtime_control_flow_depth);
            },
            [&](const ast::block_stmt& data) {
                for (const auto s : data) {
                    self(self, *s, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::expr_stmt& data) {
                self(self, *data.expression, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::discard_stmt& data) {
                self(self, *data.discarded, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::defer_stmt& data) {
                self(self, *data.deferred, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::errdefer_stmt& data) {
                self(self, *data.deferred, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::decl_stmt& data) {
                if (data.value) {
                    self(self, **data.value, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::if_expr& data) {
                if (data.condition) {
                    self(self, **data.condition, loop_depth, runtime_control_flow_depth);
                }
                const usize next_depth{runtime_control_flow_depth +
                                       (data.comptime_condition ? 0 : 1)};
                self(self, *data.consequence, loop_depth, next_depth);
                if (data.alternate) { self(self, *data.alternate, loop_depth, next_depth); }
            },
            [&](const ast::match_expr& data) {
                self(self, *data.matcher, loop_depth, runtime_control_flow_depth);
                const usize next_depth{runtime_control_flow_depth + (data.is_comptime ? 0 : 1)};
                for (const auto& arm : data.arms) {
                    self(self, *arm.dispatch, loop_depth, next_depth);
                }
            },
            [&](const ast::binary_expr& data) {
                self(self, *data.lhs, loop_depth, runtime_control_flow_depth);
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::assignment_expr& data) {
                self(self, *data.lhs, loop_depth, runtime_control_flow_depth);
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::unary_expr& data) {
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::reference_expr& data) {
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::dereference_expr& data) {
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::comptime_expr& data) {
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::address_of_expr& data) {
                self(self, *data.rhs, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::unwrap_expr& data) {
                self(self, *data.operand, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::call_expr& data) {
                self(self, *data.function, loop_depth, runtime_control_flow_depth);
                for (const auto& arg : data.arguments) {
                    if (const auto eh = arg.template as_opt<ast::expr_handle>()) {
                        self(self, **eh, loop_depth, runtime_control_flow_depth);
                    }
                }
            },
            [&](const ast::dot_expr& data) {
                self(self, *data.object, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::index_expr& data) {
                self(self, *data.array, loop_depth, runtime_control_flow_depth);
                self(self, *data.index, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::range_expr& data) {
                if (data.lhs) { self(self, **data.lhs, loop_depth, runtime_control_flow_depth); }
                if (data.rhs) { self(self, **data.rhs, loop_depth, runtime_control_flow_depth); }
            },
            [&](const ast::array_expr& data) {
                for (const auto item : data.items) {
                    self(self, *item, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const ast::label_expr& data) {
                self(self, *data.body, loop_depth, runtime_control_flow_depth);
            },
            [&](const ast::initializer_expr& data) {
                for (const auto& init : data.initializers) {
                    self(self, *init.value, loop_depth, runtime_control_flow_depth);
                }
            },
            [&](const auto&) {});
    };
    check_jumps(check_jumps, *body, 0, 0);
}

auto type_resolver::deferred_body_jumps(ast::stmt_handle body) -> bool {
    const auto diags_before{ctx_.diags.size()};
    check_deferred_body_jumps(body);
    return ctx_.diags.size() > diags_before;
}

auto type_resolver::visit(ast::node_id id, const ast::defer_stmt& defer) -> void {
    PROFILE_FUNCTION();
    // A jump out of the body is the real mistake. Resolving it would only add follow-on errors
    if (deferred_body_jumps(defer.deferred)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    TRY_RESOLVE(defer.deferred);
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::errdefer_stmt& errdef) -> void {
    PROFILE_FUNCTION();
    if (deferred_body_jumps(errdef.deferred)) {
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }

    stdx::option<type&> ret_type;
    if (!open_function_nodes_.empty()) {
        auto& fn_type{resolving_.get_sema_type(open_function_nodes_.back())};
        if (const auto fd{fn_type.get_data().as_opt<types::function>()}) {
            ret_type.emplace(fd->return_type);
        } else if (const auto cd{fn_type.get_data().as_opt<types::closure_t>()}) {
            if (const auto sd{cd->signature.get_data().as_opt<types::function>()}) {
                ret_type.emplace(sd->return_type);
            }
        }
    } else if (!return_trackers_.empty() && return_trackers_.back().expected_type) {
        ret_type.emplace(*return_trackers_.back().expected_type);
    }

    if (!ret_type) {
        ctx_.diags.emplace_back("errdefer can only be used inside a function",
                                error::UNWRAP_OUTSIDE_FUNCTION,
                                resolving_.ast.location_of(id));
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
        return;
    }

    const auto nominal_rewrap{rewrap_shape_of(ctx_, *ret_type)};
    if (!nominal_rewrap) {
        ctx_.diags.emplace_back(
            fmt::format("errdefer can only be used in a function returning a fallible type, "
                        "but '{}' does not implement 'builtin.Rewrappable'",
                        ctx_.type_display_name(*ret_type)),
            error::ERRDEFER_IN_INFALLIBLE_FN,
            resolving_.ast.location_of(id));
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
        return;
    }

    if (errdef.modifier.is_mutable_ref() || errdef.modifier.is_mutable_ptr() ||
        errdef.modifier.is_volatile() || errdef.modifier.is_poly()) {
        ctx_.diags.emplace_back("errdefer capture cannot have a mutable modifier",
                                error::ERRDEFER_MUTABLE_CAPTURE,
                                resolving_.ast.location_of(id));
    }

    if (errdef.capture && errdef.capture->is<ast::identifier_expr>() &&
        resolving_.has_sema_type(id)) {
        type* cap_type{nominal_rewrap->from_type.get()};
        if (errdef.modifier.is_ref()) {
            cap_type = &ctx_.get_reference(types::mut::CONSTANT, *cap_type);
        } else if (errdef.modifier.is_ptr()) {
            cap_type = &ctx_.get_pointer(types::mut::CONSTANT, *cap_type);
        }
        resolving_.set_sema_type(**errdef.capture, *cap_type);

        const auto& table_type{resolving_.get_sema_type(id)};
        const scope s{table_stack_, table_type.get_symbol_table_idx(), table_idx_};
        resolve_symbol_info(ast::identifier_handle{**errdef.capture}, symbol_kind::VALUE);

        TRY_RESOLVE(errdef.deferred);
    } else {
        TRY_RESOLVE(errdef.deferred);
    }

    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::VOID_));
}

auto type_resolver::visit(ast::node_id id, const ast::return_stmt& return_stmt) -> void {
    PROFILE_FUNCTION();
    if (return_stmt.expression) {
        // Make the declared return type the implicit type for the returned expression
        stdx::option<structural_guard> return_type_guard;
        if (!return_trackers_.empty() && return_trackers_.back().expected_type) {
            return_type_guard.emplace(implicit_type_stack_, *return_trackers_.back().expected_type);
        }

        TRY_RESOLVE(*return_stmt.expression);
        if (reject_loop_as_value(*return_stmt.expression)) {
            return resolving_.set_sema_type(id, *last_type_);
        }
        if (!return_trackers_.empty() && return_trackers_.back().expected_type &&
            reject_type_as_value(*return_stmt.expression, *return_trackers_.back().expected_type)) {
            return resolving_.set_sema_type(id, *last_type_);
        }
        auto& return_expr_type{*last_type_.take()};

        if (const auto local{frame_local_root(*return_stmt.expression)}) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Returning a reference into '{}', which ends with this function's "
                            "frame",
                            *local),
                error::ESCAPING_LOCAL_REFERENCE,
                resolving_.ast.location_of(*return_stmt.expression)));
        }

        // Only these two shapes are checked. The AST can't tell origin from pass-through.
        const auto constructs_closure_literal = [&] {
            if (resolving_.ast.get_as_opt<ast::function_expr>(*return_stmt.expression)) {
                return true;
            }
            const auto ident{
                resolving_.ast.get_as_opt<ast::identifier_expr>(*return_stmt.expression)};
            if (!ident) { return false; }
            const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
            if (!sym) { return false; }
            const auto node{sym->get_data().as_opt<symbols::node_t>()};
            if (!node) { return false; }
            const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
            return decl && decl->value &&
                   resolving_.ast.get_as_opt<ast::function_expr>(*decl->value);
        }();

        // A generic's captures are only known once it's instantiated, which may be later
        const auto returns_erased_early{!return_trackers_.empty() &&
                                        return_trackers_.back().expected_type &&
                                        is_fat_callable(*return_trackers_.back().expected_type)};
        if (return_expr_type.is_generic_value() &&
            (constructs_closure_literal || returns_erased_early)) {
            ctx_.generic_functions.mark_escaping(
                return_expr_type,
                {
                    .at     = resolving_.ast.location_of(*return_stmt.expression),
                    .erased = returns_erased_early,
                });
            if (report_generic_escape(return_expr_type)) {
                return last_type_.emplace(ctx_.poison_node(resolving_, id));
            }
        }

        if (constructs_closure_literal) {
            if (const auto cl{return_expr_type.get_data().as_opt<types::closure_t>()};
                cl && has_dangling_capture(*cl)) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    "Closure captures a reference into its enclosing function and cannot be "
                    "returned directly; mark it 'move fn' to capture by value instead",
                    error::ILLEGAL_CLOSURE_ESCAPE,
                    resolving_.ast.location_of(*return_stmt.expression)));
            }
        }

        // An erased callable borrows its closure's environment from this frame
        const auto returns_erased{!return_trackers_.empty() &&
                                  return_trackers_.back().expected_type &&
                                  is_fat_callable(*return_trackers_.back().expected_type)};
        if (const auto cl{return_expr_type.get_data().as_opt<types::closure_t>()};
            cl && !cl->captures.empty() && returns_erased) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                "A capturing closure erased into `fn(...)` borrows its environment from this "
                "function and cannot be returned; return it by its own type with an `auto` "
                "return instead",
                error::ILLEGAL_CLOSURE_ESCAPE,
                resolving_.ast.location_of(*return_stmt.expression)));
        }
        resolving_.set_sema_type(id, return_expr_type);
        if (!return_trackers_.empty()) { return_trackers_.back().add_return(return_expr_type); }
    } else {
        // Checked here too: a compile-time-only function never reaches the runtime checker
        if (!return_trackers_.empty() && return_trackers_.back().expected_type) {
            const auto& expected{*return_trackers_.back().expected_type};
            if (expected.get_kind() != type_kind::VOID_ && !expected.is_poison() &&
                !is_generic_type(expected)) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("Empty return in function expecting return type '{}'",
                                ctx_.type_display_name(expected)),
                    error::RETURN_TYPE_MISMATCH,
                    resolving_.ast.location_of(id)));
            }
        }
        auto& void_type{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
        resolving_.set_sema_type(id, void_type);
        if (!return_trackers_.empty()) { return_trackers_.back().add_return(void_type); }
    }
    last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::NORETURN));
}

} // namespace ghoti::sema
