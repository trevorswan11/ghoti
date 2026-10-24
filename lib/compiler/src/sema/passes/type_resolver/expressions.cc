#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/ranges.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/statement.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/peer_type.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/sema/unwrap_shape.hh"
#include "compiler/syntax/operators.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/float128.hh"
#include "support/int128.hh"
#include "support/string_utils.hh"

namespace ghoti::sema {

using namespace resolver_detail;

auto type_resolver::visit(ast::node_id id, const ast::array_expr& array) -> void {
    PROFILE_FUNCTION();

    // `[]T` / `[N]T` with no `{ ... }`: the node is a slice/array type value
    if (array.is_type_expr) {
        resolve(array.item_explicit_type);
        auto& underlying{denoted_type(*last_type_.take())};
        if (!underlying.is_resolved()) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                incomplete_array_item(resolving_.ast.location_of(array.item_explicit_type))));
        }

        const auto mutability{array_element_mutability(array.mut_elements, array.poly_elements)};
        if (array.size) {
            // Install simulated comptime_frame so array sizes can depend on preceding
            // `comptime let mut` mutations.
            const comptime_frame_guard                    sim_guard{ctx_.comptime_binding_frames,
                                                                    make_simulated_frame()};
            const auto                                    diags_before{ctx_.diags.size()};
            gir::const_eval                               evaluator{ctx_, resolving_};
            const gir::const_eval::comptime_context_guard g{evaluator, true};
            const auto                                    len_cv{evaluator.try_eval(*array.size)};
            const auto len{len_cv ? len_cv->as_uint_opt() : stdx::none};
            if (!len) {
                if (ctx_.diags.size() > diags_before) {
                    return last_type_.emplace(ctx_.poison_node(resolving_, id));
                }
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     "An array-type size must be a compile-time constant",
                                     error::COMPTIME_EVALUATION_FAILED,
                                     resolving_.ast.location_of(*array.size)));
            }
            last_type_.emplace(ctx_.get_array(
                mutability, array.null_terminated, static_cast<usize>(*len), underlying));
        } else {
            last_type_.emplace(ctx_.get_slice(mutability, array.null_terminated, underlying));
        }
        resolving_.set_sema_type(id, *last_type_);
        return;
    }

    // Resolve the element type first so each item can be typed against it
    resolve(array.item_explicit_type);
    gsl::not_null item_slot{last_type_.take()};
    // Fold sized element types so the items and the array's element slot see the concrete `[M]T`
    // rather than `type`
    item_slot = &concrete_array_type(*item_slot);
    auto& item_type{*item_slot};
    if (item_type.is_resolved() && reject_storage_slot(array.item_explicit_type, item_type)) {
        for (const auto& item : array.items) { resolve(item); }
        return last_type_.emplace(ctx_.poison_node(resolving_, id));
    }
    if (item_type.is_resolved() && item_type.get_kind() != type_kind::AUTO) {
        const structural_guard g{implicit_type_stack_, item_type};
        for (const auto& item : array.items) {
            resolve(item);
            if (reject_type_as_value(item, item_type)) {
                return resolving_.set_sema_type(id, *last_type_);
            }
        }
    } else {
        for (const auto& item : array.items) { resolve(item); }
    }

    if (!item_type.is_resolved()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            incomplete_array_item(resolving_.ast.location_of(array.item_explicit_type))));
    }

    last_type_.emplace(
        ctx_.get_array(array_element_mutability(array.mut_elements, array.poly_elements),
                       array.null_terminated,
                       array.items.size(),
                       item_type));
    resolving_.set_sema_type(id, *last_type_);
}

// The cfg pass has already evaluated this `@cfgValue`. Adopt the verdict's type
auto type_resolver::visit(ast::node_id id, const ast::cfg_value_expr&) -> void {
    PROFILE_FUNCTION();
    const auto it{resolving_.cfg_value_results.find(id.get_index())};
    if (it == resolving_.cfg_value_results.end() || it->second.is_predicate) {
        auto& bool_type{ctx_.get_builtin_resolved_type(type_kind::BOOL)};
        resolving_.set_sema_type(id, bool_type);
        last_type_.emplace(bool_type);
        return;
    }
    TRY_RESOLVE(*it->second.chosen);
    auto& chosen_type{*last_type_};
    resolving_.set_sema_type(id, chosen_type);
    last_type_.emplace(chosen_type);
}

// `@cfg` statements are spliced away by the cfg pass before type resolution runs.
auto type_resolver::visit(ast::node_id, const ast::cfg_stmt&) -> void {
    UNREACHABLE("cfg_stmt must be removed by the cfg pass before type resolution");
}

namespace {

// True only for concrete integer, or width-less `comptime_int`
[[nodiscard]] auto wrapping_operand_ok(const type& t) noexcept -> bool {
    return is_integer(t.get_kind()) || t.get_kind() == type_kind::COMPTIME_INT;
}

// The element type of an array/slice, or none if `t` is neither (for `++`, `PLUS_PLUS`)
[[nodiscard]] auto concat_element_type(type& t) noexcept -> stdx::option<type&> {
    if (const auto arr{t.get_data().as_opt<types::array>()}) { return arr->underlying; }
    if (const auto sl{t.get_data().as_opt<types::slice>()}) { return sl->underlying; }
    return stdx::none;
}

[[nodiscard]] auto concat_is_null_terminated(const type& t) noexcept -> bool {
    if (const auto arr{t.get_data().as_opt<types::array>()}) { return arr->null_terminated; }
    if (const auto sl{t.get_data().as_opt<types::slice>()}) { return sl->null_terminated; }
    return false;
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::assignment_expr& assign) -> void {
    PROFILE_FUNCTION();
    {
        const mutating_context_guard g{in_mutating_context_};
        TRY_RESOLVE(assign.lhs);
    }
    auto& lhs_type{*last_type_.take()};
    if (const auto dest{slice_copy_destination(resolving_, assign.lhs)}) {
        return resolve_slice_copy(id, assign, *dest);
    }
    if (!is_lvalue_expression(resolving_, assign.lhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot assign to a temporary value; the left side must be a "
                             "variable, field, element, or dereference",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(assign.lhs)));
    }
    // An array's `.len` and `.ptr` are read-only (a `let mut` slice's are assignable)
    if (const auto dot{resolving_.ast.get_as_opt<ast::dot_expr>(assign.lhs)};
        dot && structural_members_.contains(ast::node_id{dot->member}.get_index())) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("'.{}' of an array is read-only",
                        resolving_.ast.get_as<ast::identifier_expr>(dot->member).name),
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(assign.lhs)));
    }
    if (const auto ident{resolving_.ast.get_as_opt<ast::identifier_expr>(assign.lhs)}) {
        // A parameter only becomes `comptime` by a read elsewhere, which the store has to name
        if (const auto param{inferred_comptime_param(ident->name)}) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Cannot assign to '{}', which is a `comptime` parameter{}",
                            ident->name,
                            inferred_comptime_reason(resolving_, *param)),
                error::ASSIGNMENT_TO_CONST,
                resolving_.ast.location_of(assign.lhs)));
        }

        // A `comptime let mut` type takes any other type
        const auto sym{ctx_.registry.lookup(table_stack_, ident->name)};
        if (sym && comptime_type_var_value(*sym)) {
            TRY_RESOLVE(assign.rhs);
            if (id.get_token_type() != syntax::token_type_t::ASSIGN ||
                operand_nature(assign.rhs) != operand_nature_t::TYPE) {
                return last_type_.emplace(
                    ctx_.poison_node(resolving_,
                                     id,
                                     fmt::format("'{}' holds a type, so only a type can be "
                                                 "assigned to it",
                                                 ident->name),
                                     error::TYPE_MISMATCH,
                                     resolving_.ast.location_of(assign.rhs)));
            }
            auto& void_t{ctx_.get_builtin_resolved_type(type_kind::VOID_)};
            resolving_.set_sema_type(id, void_t);
            return last_type_.emplace(void_t);
        }
    }
    {
        // Assigning to a reference writes its referent, so that is what the value infers as
        auto&                  dest_type{lhs_type.get_kind() == type_kind::REFERENCE
                                             ? lhs_type.get_data().as<types::reference>().underlying
                                             : lhs_type};
        const structural_guard g{implicit_type_stack_, *ctx_.pool.strip_volatile(dest_type)};
        TRY_RESOLVE(assign.rhs);
    }
    if (reject_type_as_value(assign.rhs, lhs_type)) {
        return resolving_.set_sema_type(id, *last_type_);
    }
    auto& rhs_type{*last_type_};

    switch (id.get_token_type()) {
    case syntax::token_type_t::PLUS_PERCENT_ASSIGN:
    case syntax::token_type_t::MINUS_PERCENT_ASSIGN:
    case syntax::token_type_t::STAR_PERCENT_ASSIGN:
    case syntax::token_type_t::SHL_PERCENT_ASSIGN:
    case syntax::token_type_t::PLUS_PIPE_ASSIGN:
    case syntax::token_type_t::MINUS_PIPE_ASSIGN:
    case syntax::token_type_t::STAR_PIPE_ASSIGN:
    case syntax::token_type_t::SHL_PIPE_ASSIGN:
        if (!lhs_type.is_poison() && !rhs_type.is_poison() &&
            (!wrapping_operand_ok(lhs_type) || !wrapping_operand_ok(rhs_type))) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("operator '{}' expects two integer operands; found '{}' and '{}'",
                            *syntax::get_operator_opt(id.get_token_type()),
                            ctx_.type_display_name(lhs_type),
                            ctx_.type_display_name(rhs_type)),
                error::OPERATOR_TYPE_MISMATCH,
                resolving_.ast.location_of(id)));
        }
        break;
    default: break;
    }

    // Only pass 3 can verify assignment allowance due to mutability semantics
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::inferred_comptime_param(std::string_view name) const
    -> stdx::option<const ast::function_expr::parameter&> {
    auto fn{instantiating_fn_};
    if (!open_function_nodes_.empty()) {
        const auto open{resolving_.ast.get_as_opt<ast::function_expr>(open_function_nodes_.back())};
        fn = open;
    }
    if (!fn) { return stdx::none; }
    for (const auto& param : fn->parameters) {
        if (!param.is_comptime || param.is_comptime_written ||
            !param.name.is<ast::identifier_expr>()) {
            continue;
        }
        if (resolving_.ast.get_as<ast::identifier_expr>(param.name).name == name) { return param; }
    }
    return stdx::none;
}

auto type_resolver::resolve_slice_copy(ast::node_id                id,
                                       const ast::assignment_expr& assign,
                                       ast::expr_handle            dest) -> void {
    auto&      dest_type{resolving_.get_sema_type(dest)};
    auto&      dest_elem{dest_type.get_data().as<types::slice>().underlying};
    const auto fail{[&](std::string message, error err, ast::node_id at) {
        last_type_.emplace(ctx_.poison_node(
            resolving_, id, std::move(message), err, resolving_.ast.location_of(at)));
    }};

    if (id.get_token_type() != syntax::token_type_t::ASSIGN) {
        return fail(
            "A slice range can only be the target of a plain `=` copy", error::TYPE_MISMATCH, id);
    }
    if (dest_type.is_constant()) {
        return fail("Cannot copy into a slice of immutable elements; the destination needs `mut` "
                    "elements",
                    error::ASSIGNMENT_TO_CONST,
                    assign.lhs);
    }
    const auto dest_len{known_length(dest)};
    if (!dest_len) {
        return fail("Cannot copy into a slice whose length is not known at compile time; slice it "
                    "with constant bounds first (e.g. `s[i..][0..n]`)",
                    error::UNKNOWN_SLICE_LENGTH,
                    assign.lhs);
    }

    // `.{...}` on the right infers as an array of the destination's length
    {
        auto& expected{ctx_.get_array(types::mut::CONSTANT, false, *dest_len, dest_elem)};
        const structural_guard g{implicit_type_stack_, expected};
        TRY_RESOLVE(assign.rhs);
    }
    auto& rhs_type{*last_type_.take()};

    gsl::not_null src_type{&rhs_type};
    if (const auto ref{src_type->get_data().as_opt<types::reference>()}) {
        src_type = &ref->underlying;
    }
    src_type = &concrete_array_type(*src_type);

    stdx::option<u64>         src_len;
    stdx::option<const type&> src_elem;
    if (const auto arr{src_type->get_data().as_opt<types::array>()}) {
        src_len  = arr->len;
        src_elem = arr->underlying;
    } else if (const auto slice{src_type->get_data().as_opt<types::slice>()}) {
        src_len  = known_length(assign.rhs);
        src_elem = slice->underlying;
    } else {
        return fail(fmt::format("Cannot copy '{}' into a slice; expected an array or slice",
                                ctx_.type_display_name(rhs_type)),
                    error::TYPE_MISMATCH,
                    assign.rhs);
    }

    if (!is_same_unqualified(*src_elem, dest_elem)) {
        return fail(fmt::format("Cannot copy '{}' elements into a slice of '{}'",
                                ctx_.type_display_name(*src_elem),
                                ctx_.type_display_name(dest_elem)),
                    error::TYPE_MISMATCH,
                    assign.rhs);
    }
    if (!src_len) {
        return fail("Cannot copy from a slice whose length is not known at compile time; slice it "
                    "with constant bounds first (e.g. `s[i..][0..n]`)",
                    error::UNKNOWN_SLICE_LENGTH,
                    assign.rhs);
    }
    if (*src_len != *dest_len) {
        return fail(
            fmt::format("Cannot copy {} elements into a slice of length {}", *src_len, *dest_len),
            error::SLICE_LENGTH_MISMATCH,
            assign.rhs);
    }

    resolving_.set_sema_type(id, rhs_type);
    last_type_.emplace(rhs_type);
}

auto type_resolver::visit(ast::node_id id, const ast::binary_expr& binary) -> void {
    PROFILE_FUNCTION();
    // A reference operand is read through, as the emitter loads it
    const auto referent_view = [](type& t) -> type& {
        if (const auto ref{t.get_data().as_opt<types::reference>()}) { return ref->underlying; }
        return t;
    };

    // `.a == x`: the right side names what `.a` is a member of, so it resolves first
    stdx::option<type&> lhs_first;
    stdx::option<type&> rhs_first;
    if (resolving_.ast.get_as_opt<ast::implicit_access_expr>(binary.lhs)) {
        {
            // Whatever context surrounds the comparison doesn't describe its right side
            const structural_guard g{implicit_type_stack_,
                                     ctx_.get_builtin_resolved_type(type_kind::ENUM_LITERAL)};
            TRY_RESOLVE(binary.rhs);
        }
        rhs_first = referent_view(*last_type_.take());
        {
            const structural_guard g{implicit_type_stack_, *ctx_.pool.strip_volatile(*rhs_first)};
            TRY_RESOLVE(binary.lhs);
        }
        lhs_first = referent_view(*last_type_.take());
    } else {
        TRY_RESOLVE(binary.lhs);
        lhs_first = referent_view(*last_type_.take());
        // `false and x` / `true or x` never evaluates `x`, so nothing it would report applies
        if (lhs_decides_logical_op(id.get_token_type(), binary.lhs)) {
            const auto diags_before{ctx_.diags.size()};
            resolve(binary.rhs);
            DISCARD(ctx_.diags.split_off(diags_before));
            auto& bool_type{ctx_.get_builtin_resolved_type(type_kind::BOOL)};
            resolving_.set_sema_type(id, bool_type);
            return last_type_.emplace(bool_type);
        }
        {
            const structural_guard g{implicit_type_stack_, *ctx_.pool.strip_volatile(*lhs_first)};
            TRY_RESOLVE(binary.rhs);
        }
        rhs_first = referent_view(*last_type_.take());
    }
    gsl::not_null<type*> lhs_type{lhs_first.get()};
    auto&                rhs_type{*rhs_first};

    // A range counter on the left takes the right side's type when it holds every value
    const auto shift_token{id.get_token_type()};
    if (shift_token != syntax::token_type_t::SHL && shift_token != syntax::token_type_t::SHR &&
        shift_token != syntax::token_type_t::SHL_PERCENT &&
        shift_token != syntax::token_type_t::SHL_PIPE && is_integer(lhs_type->get_kind()) &&
        !is_same_unqualified(*lhs_type, rhs_type) && adopt_counter_type(binary.lhs, rhs_type)) {
        lhs_type = &resolving_.get_sema_type(binary.lhs);
    }

    // A shift's RHS is a bit count, not a peer value of the LHS being shifted
    const auto op{id.get_token_type()};

    for (const auto& [operand, operand_type] :
         {std::pair{binary.lhs, lhs_type},
          std::pair{binary.rhs, gsl::not_null<type*>{&rhs_type}}}) {
        if (operand_type->get_kind() != type_kind::UNDEFINED) { continue; }
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("operator '{}' has no value to read in 'undefined'",
                        syntax::get_operator_opt(op).value_or(std::string_view{"?"})),
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(operand)));
    }

    // Types only exist at compile time and only compare for identity against other types
    const auto lhs_nature{operand_nature(binary.lhs)};
    const auto rhs_nature{lhs_nature == operand_nature_t::UNKNOWN ? operand_nature_t::UNKNOWN
                                                                  : operand_nature(binary.rhs)};
    const bool involves_type{lhs_nature == operand_nature_t::TYPE ||
                             rhs_nature == operand_nature_t::TYPE};
    const bool fully_known{lhs_nature != operand_nature_t::UNKNOWN &&
                           rhs_nature != operand_nature_t::UNKNOWN};
    const bool is_type_identity_check{
        lhs_nature == rhs_nature &&
        (op == syntax::token_type_t::EQ || op == syntax::token_type_t::NEQ)};
    if (involves_type && fully_known && !is_type_identity_check) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("operator '{}' cannot be applied to a type; types only compare with "
                        "'==' or '!=' against other types",
                        syntax::get_operator_opt(op).value_or(std::string_view{"?"})),
            error::TYPE_USED_AS_VALUE,
            resolving_.ast.location_of(id)));
    }
    const bool is_shift_op{op == syntax::token_type_t::SHL || op == syntax::token_type_t::SHR ||
                           op == syntax::token_type_t::SHL_PERCENT ||
                           op == syntax::token_type_t::SHL_PIPE};
    if (is_vector(*lhs_type) || is_vector(rhs_type)) {
        return resolve_vector_binary(id, binary, *lhs_type, rhs_type);
    }

    // Two numbers meet at their peer type. An untyped constant takes it on
    const bool peers_operands{!is_shift_op && op != syntax::token_type_t::PLUS_PLUS &&
                              op != syntax::token_type_t::BOOLEAN_AND &&
                              op != syntax::token_type_t::BOOLEAN_OR};
    if (peers_operands && is_numeric(lhs_type->get_kind()) && is_numeric(rhs_type.get_kind())) {
        const std::array operands{peer_operand{lhs_type}, peer_operand{&rhs_type}};
        const auto       peer{peer_type(ctx_, operands)};
        if (!peer) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 peer_error_message(ctx_, operands, peer.error()),
                                 error::NO_PEER_TYPE,
                                 resolving_.ast.location_of(id)));
        }
        if (is_comptime_numeric(lhs_type->get_kind())) {
            resolving_.set_sema_type(binary.lhs, **peer);
        }
        if (is_comptime_numeric(rhs_type.get_kind())) {
            resolving_.set_sema_type(binary.rhs, **peer);
        }
        lhs_type = peer->get();
    }

    switch (id.get_token_type()) {
    case syntax::token_type_t::PLUS_PLUS:   resolve_concat(id, binary, *lhs_type, rhs_type); break;
    case syntax::token_type_t::LT:
    case syntax::token_type_t::LT_EQ:
    case syntax::token_type_t::GT:
    case syntax::token_type_t::GT_EQ:
    case syntax::token_type_t::EQ:
    case syntax::token_type_t::NEQ:
    case syntax::token_type_t::BOOLEAN_AND:
    case syntax::token_type_t::BOOLEAN_OR:
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::BOOL));
        break;
    case syntax::token_type_t::PLUS_PERCENT:
    case syntax::token_type_t::MINUS_PERCENT:
    case syntax::token_type_t::STAR_PERCENT:
    case syntax::token_type_t::SHL_PERCENT:
    case syntax::token_type_t::PLUS_PIPE:
    case syntax::token_type_t::MINUS_PIPE:
    case syntax::token_type_t::STAR_PIPE:
    case syntax::token_type_t::SHL_PIPE:      {
        if (!lhs_type->is_poison() && !rhs_type.is_poison() &&
            (!wrapping_operand_ok(*lhs_type) || !wrapping_operand_ok(rhs_type))) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("operator '{}' expects two integer operands; found '{}' and '{}'",
                            *syntax::get_operator_opt(id.get_token_type()),
                            ctx_.type_display_name(*lhs_type),
                            ctx_.type_display_name(rhs_type)),
                error::OPERATOR_TYPE_MISMATCH,
                resolving_.ast.location_of(id)));
        }
        last_type_.emplace(lhs_type);
        break;
    }
    case syntax::token_type_t::PLUS:
        // `n + ptr` offsets the pointer just like `ptr + n`
        last_type_.emplace(
            (is_integer(lhs_type->get_kind()) || lhs_type->get_kind() == type_kind::COMPTIME_INT) &&
                    rhs_type.get_kind() == type_kind::POINTER
                ? rhs_type
                : *lhs_type);
        break;
    default: last_type_.emplace(lhs_type); break;
    }

    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::scalar_broadcasts(ast::expr_handle scalar, type& scalar_type, type& lane)
    -> bool {
    if (scalar_type.is_poison()) { return true; }
    if (scalar_type.get_kind() == type_kind::COMPTIME_FLOAT && !is_float(lane.get_kind())) {
        return false;
    }
    if (is_comptime_numeric(scalar_type.get_kind())) {
        // The constant is typed as the lane from here on, so it has to fit one
        gir::const_eval evaluator{ctx_, resolving_};
        if (const auto folded{evaluator.try_eval(scalar)}; folded && !folded->is_poison()) {
            const auto as_int{folded->as_int_opt()};
            const auto as_big{folded->as_opt<big_int>()};
            const auto as_float{folded->as_opt<f128>()};
            const bool fits{as_int   ? comptime_int_fits(*as_int, lane, target_ptr_bits())
                            : as_big ? comptime_int_fits(*as_big, lane, target_ptr_bits())
                            : as_float && is_float(lane.get_kind())
                                ? comptime_float_fits(*as_float, lane)
                                : true};
            if (!fits) {
                ctx_.diags.emplace_back(fmt::format("{} literal is out of range for type '{}'",
                                                    as_float ? "float" : "integer",
                                                    ctx_.type_display_name(lane)),
                                        error::LITERAL_OUT_OF_RANGE,
                                        resolving_.ast.location_of(scalar));
            }
        }
        resolving_.set_sema_type(scalar, lane);
        return true;
    }
    return is_assignable(scalar_type, lane);
}

auto type_resolver::resolve_vector_binary(ast::node_id            id,
                                          const ast::binary_expr& binary,
                                          type&                   lhs_type,
                                          type&                   rhs_type) -> void {
    const auto op{id.get_token_type()};
    const auto op_name{syntax::get_operator_opt(op).value_or(std::string_view{"?"})};
    const auto fail{[&](std::string message, ast::node_id at) {
        last_type_.emplace(ctx_.poison_node(resolving_,
                                            id,
                                            std::move(message),
                                            error::OPERATOR_TYPE_MISMATCH,
                                            resolving_.ast.location_of(at)));
    }};
    if (op == syntax::token_type_t::PLUS_PLUS) {
        return fail("operator '++' does not apply to vectors", id);
    }

    const bool is_shift{op == syntax::token_type_t::SHL || op == syntax::token_type_t::SHR ||
                        op == syntax::token_type_t::SHL_PERCENT ||
                        op == syntax::token_type_t::SHL_PIPE};
    auto&      shape{is_vector(lhs_type) ? lhs_type : rhs_type};
    const auto len{vector_of(shape)->len};

    // The vector a scalar side stands for: its own type's lanes for a shift, else the other side's
    const auto scalar_lanes{[&](type& scalar_type) -> type& {
        if (!is_shift || is_comptime_numeric(scalar_type.get_kind())) {
            return lane_type_of(shape);
        }
        return *ctx_.pool.with_const(scalar_type, true);
    }};
    for (const auto& [operand, operand_type] :
         {std::pair{binary.lhs, &lhs_type}, std::pair{binary.rhs, &rhs_type}}) {
        if (is_vector(*operand_type)) { continue; }
        auto& lane{scalar_lanes(*operand_type)};
        if (!scalar_broadcasts(operand, *operand_type, lane)) {
            return fail(fmt::format("'{}' does not broadcast to the lanes of '{}'",
                                    ctx_.type_display_name(*operand_type),
                                    ctx_.type_display_name(shape)),
                        operand);
        }
    }

    auto& value_lane{is_vector(lhs_type) ? lane_type_of(lhs_type) : scalar_lanes(lhs_type)};
    auto& value_type{ctx_.get_vector(types::mut::CONSTANT, len, value_lane)};
    auto& bool_type{ctx_.get_builtin_resolved_type(type_kind::BOOL)};
    switch (op) {
    case syntax::token_type_t::LT:
    case syntax::token_type_t::LT_EQ:
    case syntax::token_type_t::GT:
    case syntax::token_type_t::GT_EQ:
    case syntax::token_type_t::EQ:
    case syntax::token_type_t::NEQ:
        last_type_.emplace(ctx_.with_lane(value_type, bool_type));
        break;
    case syntax::token_type_t::BOOLEAN_AND:
    case syntax::token_type_t::BOOLEAN_OR:
        if (value_lane.get_kind() != type_kind::BOOL) {
            return fail(fmt::format("operator '{}' needs 'bool' lanes; found '{}'",
                                    op_name,
                                    ctx_.type_display_name(value_type)),
                        id);
        }
        last_type_.emplace(value_type);
        break;
    case syntax::token_type_t::PLUS_PERCENT:
    case syntax::token_type_t::MINUS_PERCENT:
    case syntax::token_type_t::STAR_PERCENT:
    case syntax::token_type_t::SHL_PERCENT:
    case syntax::token_type_t::PLUS_PIPE:
    case syntax::token_type_t::MINUS_PIPE:
    case syntax::token_type_t::STAR_PIPE:
    case syntax::token_type_t::SHL_PIPE:
        if (!is_integer(value_lane.get_kind())) {
            return fail(fmt::format("operator '{}' needs integer lanes; found '{}'",
                                    op_name,
                                    ctx_.type_display_name(value_type)),
                        id);
        }
        last_type_.emplace(value_type);
        break;
    default: last_type_.emplace(value_type); break;
    }
    resolving_.set_sema_type(id, *last_type_);
}

// `lhs ++ rhs`: compile-time array/slice concatenation. Result is `[N+M]T`, sentinel from `rhs`.
auto type_resolver::resolve_concat(ast::node_id            id,
                                   const ast::binary_expr& binary,
                                   type&                   lhs_type_in,
                                   type&                   rhs_type_in) -> void {
    // An identifier naming an array value can still carry a `TYPE`-wrapped meta-type or a
    // not-yet-concrete `deferred_array` (e.g. a literal array dimension). Unwrap/force both.
    gir::const_eval evaluator{ctx_, resolving_};
    auto&           lhs_type{evaluator.force_deferred_array(denoted_type(lhs_type_in))};
    auto&           rhs_type{evaluator.force_deferred_array(denoted_type(rhs_type_in))};
    const auto      lhs_elem{concat_element_type(lhs_type)};
    const auto      rhs_elem{concat_element_type(rhs_type)};
    if (!lhs_elem || !rhs_elem) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("operator '++' requires two array or slice operands; found '{}' and '{}'",
                        ctx_.type_display_name(lhs_type),
                        ctx_.type_display_name(rhs_type)),
            error::CONCAT_REQUIRES_ARRAY_OR_SLICE,
            resolving_.ast.location_of(id)));
    }
    if (!is_same_unqualified(*lhs_elem, *rhs_elem)) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("operator '++' requires matching element types; found '{}' and '{}'",
                        ctx_.type_display_name(*lhs_elem),
                        ctx_.type_display_name(*rhs_elem)),
            error::CONCAT_ELEM_TYPE_MISMATCH,
            resolving_.ast.location_of(id)));
    }

    const auto lhs_len{fold_concat_operand_len(binary.lhs, lhs_type)};
    const auto rhs_len{fold_concat_operand_len(binary.rhs, rhs_type)};
    if (!lhs_len || !rhs_len) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "operands of '++' must be known at compile time",
                             error::CONCAT_NOT_FOLDABLE,
                             resolving_.ast.location_of(!lhs_len ? binary.lhs : binary.rhs)));
    }

    last_type_.emplace(ctx_.get_array(
        types::mut::CONSTANT, concat_is_null_terminated(rhs_type), *lhs_len + *rhs_len, *lhs_elem));
}

// The element count of a `++` operand: read off an array type directly, or fold a slice value.
auto type_resolver::fold_concat_operand_len(ast::expr_handle operand, type& operand_type)
    -> stdx::option<usize> {
    if (const auto arr{operand_type.get_data().as_opt<types::array>()}) { return arr->len; }

    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(operand)};
    if (!folded) { return stdx::none; }
    if (const auto arr{folded->as_opt<gir::const_array>()}) { return arr->elements.size(); }
    if (const auto str{folded->as_opt<std::string>()}) { return str->size(); }
    return stdx::none;
}

auto type_resolver::validate_struct_initializer(ast::node_id                 init_id,
                                                const ast::initializer_expr& init,
                                                type&                        struct_type)
    -> stdx::result<void, diagnostic> {
    struct_validator_.clear();
    const auto struct_data_opt{struct_type.get_data().as_opt<types::struct_t>()};
    if (!struct_data_opt) {
        return diagnostic{"Initializer target is not a struct",
                          error::TYPE_MISMATCH,
                          resolving_.ast.location_of(init_id)};
    }
    const auto& struct_data{*struct_data_opt};

    // Check for duplicates
    for (const auto& [accessor_opt, value] : init.initializers) {
        if (!accessor_opt) {
            return diagnostic{"Struct initializers require '.field = ...' entries",
                              error::TYPE_MISMATCH,
                              resolving_.ast.location_of(value)};
        }
        const auto  accessor_node{resolving_.ast.get_as<ast::implicit_access_expr>(*accessor_opt)};
        const auto& accessor_ident =
            resolving_.ast.get_as<ast::identifier_expr>(accessor_node.member);
        const auto field_name{accessor_ident.name};
        if (!struct_validator_.seen.insert(field_name).second) {
            struct_validator_.duplicates.emplace_back(field_name);
        }
        struct_validator_.provided.emplace_back(field_name);
    }

    if (!struct_validator_.duplicates.empty()) {
        return diagnostic{
            fmt::format("Struct initializer contains duplicate field{}: {}",
                        string_utils::plural_suffix(struct_validator_.duplicates.size()),
                        fmt::join(struct_validator_.duplicates, ", ")),
            error::DUPLICATE_FIELD,
            resolving_.ast.location_of(init_id)};
    }

    // Check for missing fields
    const auto& enclosing{struct_data.enclosing};
    for (const auto& [ident, _1, default_value, _2] : struct_data.ast_fields) {
        const auto& field_node{enclosing.ast.get_as<ast::identifier_expr>(ident)};
        if (!default_value && !struct_validator_.seen.contains(field_node.name)) {
            struct_validator_.missings.emplace_back(field_node.name);
        }
    }

    if (!struct_validator_.missings.empty()) {
        return diagnostic{
            fmt::format("Struct initializer missing required field{}: {}",
                        string_utils::plural_suffix(struct_validator_.missings.size()),
                        fmt::join(struct_validator_.missings, ", ")),
            error::MISSING_FIELD,
            resolving_.ast.location_of(init_id)};
    }

    // Check for extra/unknown fields
    const auto  struct_table_idx{struct_type.get_symbol_table_idx()};
    const auto& struct_table{ctx_.registry.get(struct_table_idx)};
    for (const auto name : struct_validator_.provided) {
        if (!struct_table.has(name)) { struct_validator_.unknowns.emplace_back(name); }
    }

    if (!struct_validator_.unknowns.empty()) {
        return diagnostic{
            fmt::format("Struct initializer contains unknown field{}: {}",
                        string_utils::plural_suffix(struct_validator_.unknowns.size()),
                        fmt::join(struct_validator_.unknowns, ", ")),
            error::UNKNOWN_FIELD,
            resolving_.ast.location_of(init_id)};
    }
    return {};
}

auto type_resolver::visit(ast::node_id id, const ast::initializer_expr& init) -> void {
    PROFILE_FUNCTION();

    // Resolve the object first so it can be tied to the member's types
    stdx::option<type&> object_type_opt;
    if (init.object_type) {
        TRY_RESOLVE(*init.object_type);
        object_type_opt.emplace(*last_type_.take());
    } else if (const auto implicit_type{implicit_type_stack_.peek()}) {
        object_type_opt.emplace(*implicit_type);
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Initializer expression requires a known type; provide an explicit "
                             "type or use in a typed context",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    if (object_type_opt->get_data().is<types::deferred_call>()) {
        gir::const_eval evaluator{ctx_, resolving_};
        object_type_opt.emplace(denoted_type(evaluator.force_deferred_call(*object_type_opt)));
    }

    type& object_type{*object_type_opt};
    if (!object_type.is_resolved()) {
        return last_type_.emplace(ctx_.poison_node(resolving_,
                                                   id,
                                                   "Cannot initialize an incomplete type",
                                                   error::CYCLIC_DEPENDENCY,
                                                   resolving_.ast.location_of(id)));
    }

    const auto  num_initializers{init.initializers.size()};
    const auto& object_data{object_type.get_data()};

    // `RowAlias{ a, b, c }` / `.{ a, b }` in an array-typed context
    gsl::not_null<type*> array_object{&object_type};
    if (const auto deferred{object_data.as_opt<types::deferred_array>()}) {
        array_object = &concrete_array_type(object_type);
        // `[n]T` sized by a parameter of the call being resolved: the values give the length,
        // which the instantiation then checks against `n`
        if (array_object->get_data().is<types::deferred_array>()) {
            array_object = &ctx_.get_array(object_type.get_key().get_mut(),
                                           deferred->array.null_terminated,
                                           num_initializers,
                                           deferred->underlying);
        }
    }
    // An array's elements, or a vector's lanes, as positional values
    const auto positional{[&] -> stdx::option<std::pair<type&, usize>> {
        if (const auto arr_data{array_object->get_data().as_opt<types::array>()}) {
            return std::pair<type&, usize>{arr_data->underlying, arr_data->len};
        }
        if (const auto vec{vector_of(*array_object)}) {
            return std::pair<type&, usize>{vec->underlying, usize{vec->len}};
        }
        return stdx::none;
    }()};
    if (positional) {
        auto&      element_type{positional->first};
        const auto len{positional->second};
        const auto kind_name{is_vector(*array_object) ? "A vector" : "An array"};
        for (const auto& entry : init.initializers) {
            if (entry.member) {
                return last_type_.emplace(ctx_.poison_node(
                    resolving_,
                    id,
                    fmt::format("{} initializer takes positional values, not '.field = ...' "
                                "entries",
                                kind_name),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(*entry.member)));
            }
        }
        if (num_initializers != len) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("{} of length {} initialized with {} value(s)",
                                             is_vector(*array_object) ? "Vector" : "Array",
                                             len,
                                             num_initializers),
                                 error::ARITY_MISMATCH,
                                 resolving_.ast.location_of(id)));
        }
        for (const auto& entry : init.initializers) {
            const structural_guard g{implicit_type_stack_, element_type};
            TRY_RESOLVE(entry.value);
            if (reject_type_as_value(entry.value, element_type)) {
                return resolving_.set_sema_type(id, *last_type_);
            }
        }
        resolving_.set_sema_type(id, *array_object);
        return last_type_.emplace(*array_object);
    }

    if (object_data.is<types::enum_t>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Enums cannot be initialized with an initializer expression as "
                             "they lack member variables",
                             error::ARITY_MISMATCH,
                             resolving_.ast.location_of(id)));
    }
    if (object_data.is<types::union_t>()) {
        // This is a restriction naturally imposed by the definition of a union in theory
        if (num_initializers != 1) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                fmt::format("Union initializer lists must list exactly one field; found {}",
                            num_initializers),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(id)));
        }
    } else if (const auto struct_data{object_data.as_opt<types::struct_t>()}) {
        if (auto valid{validate_struct_initializer(id, init, object_type)}; !valid) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id, std::move(valid).error()));
        }
    }

    if (!object_data.is<types::struct_t>() && !object_data.is<types::union_t>()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Only struct and union types may be used in "
                                         "initializer expressions; found '{}'",
                                         ctx_.type_display_name(object_type)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }

    for (const auto& [accessor_opt, value] : init.initializers) {
        if (!accessor_opt) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "Struct and union initializers require '.field = ...' entries",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(value)));
        }
        const auto accessor{*accessor_opt};

        // The accessor is always a lookup into the stack
        {
            const structural_guard       g{implicit_type_stack_, object_type};
            const mutating_context_guard accessor_g{resolving_init_accessor_, true};
            TRY_RESOLVE(accessor);
        }

        // The value might be a necessary implicit access
        auto& member_type{resolving_.get_sema_type(accessor)};
        if (member_type.is_poison()) {
            return last_type_.emplace(ctx_.poison_node(resolving_, id));
        }

        {
            const structural_guard g{implicit_type_stack_, member_type};
            TRY_RESOLVE(value);
        }
        if (reject_type_as_value(value, member_type)) {
            return resolving_.set_sema_type(id, *last_type_);
        }
    }

    resolving_.set_sema_type(id, object_type);
    last_type_.emplace(object_type);
}

namespace {

// Returns the mutability associated with the reference/address-of operator
[[nodiscard]] auto ref_addr_of_is_mutable(ast::node_id id) noexcept -> types::mutability_modifiers {
    using syntax::token_type_t;
    switch (id.get_token_type()) {
    case token_type_t::BW_AND:         return types::mut::CONSTANT;
    case token_type_t::AND_MUT:        return types::mut::MUTABLE;
    case token_type_t::CARET:          return types::mut::CONSTANT;
    case token_type_t::CARET_MUT:      return types::mut::MUTABLE;
    case token_type_t::AND_MUT_POLY:
    case token_type_t::CARET_MUT_POLY: return types::mut::POLY;
    default: UNREACHABLE("Invalid token types should be pruned prior to this function");
    }
}

// `&p.field` / `^p.field` is rejected on packed aggregates as they are just integers
[[nodiscard]] auto rhs_is_packed_field(const mod::module_t& mod, auto rhs) -> bool {
    const auto dot{mod.ast.template get_as_opt<ast::dot_expr>(rhs)};
    if (!dot) { return false; }
    const auto obj_t{mod.get_sema_type_opt(dot->object)};
    if (!obj_t) { return false; }
    gsl::not_null<const type*> ot{obj_t.get()};
    if (const auto p{ot->get_data().as_opt<types::pointer>()}) {
        ot = &p->underlying;
    } else if (const auto r{ot->get_data().as_opt<types::reference>()}) {
        ot = &r->underlying;
    }
    if (const auto st{ot->get_data().as_opt<types::struct_t>()}) { return st->is_bit_packed(); }
    if (const auto ut{ot->get_data().as_opt<types::union_t>()}) { return ut->is_bit_packed(); }
    return false;
}

// `&v[i]` / `^v[i]`: a vector's lanes live in one register-sized value, not addressable slots
[[nodiscard]] auto rhs_is_vector_lane(const mod::module_t& mod, auto rhs) -> bool {
    const auto index{mod.ast.template get_as_opt<ast::index_expr>(rhs)};
    if (!index) { return false; }
    const auto obj_t{mod.get_sema_type_opt(index->array)};
    if (!obj_t) { return false; }
    // `p[i]` through a pointer steps over whole vectors, which do have addresses
    const auto ref{obj_t->get_data().template as_opt<types::reference>()};
    return is_vector(ref ? ref->underlying : *obj_t);
}

} // namespace

namespace {

// `&.a` / `^.a` with nothing naming the enum: the literal only exists at compile time
[[nodiscard]] auto enum_literal_has_no_address(source_location loc) -> diagnostic {
    return diagnostic{"an enum literal has no address; give it its enum type first, as in "
                      "`const e: E = .a;`",
                      error::TYPE_MISMATCH,
                      loc};
}

} // namespace

auto type_resolver::visit(ast::node_id id, const ast::reference_expr& ref) -> void {
    PROFILE_FUNCTION();
    {
        const mutating_context_guard g{in_mutating_context_,
                                       ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
        const mutating_context_guard dyn_g{dyn_unsized_ok_, ref.rhs.is<ast::type_expr>()};
        TRY_RESOLVE(ref.rhs);
    }
    auto& rhs_type{*last_type_.take()};
    if (rhs_type.get_kind() == type_kind::ENUM_LITERAL) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_, id, enum_literal_has_no_address(resolving_.ast.location_of(id))));
    }

    if (names_comptime_mut(ref.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take a reference to a `comptime let mut`; it has no storage",
                             error::COMPTIME_MUT_ADDRESS_OF,
                             resolving_.ast.location_of(id)));
    }

    // Semantic safeguard: disallow taking a mutable reference to an rvalue/temporary
    // (e.g. `&mut 42`), which would immediately become a dangling reference to dropped storage.
    const bool is_mut{ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
    if (is_mut && !is_type_denoting_expr(ctx_, resolving_, ref.rhs, &table_stack_) &&
        !is_lvalue_expression(resolving_, ref.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take a mutable reference to a temporary value",
                             error::ILLEGAL_RVALUE_CAPTURE,
                             resolving_.ast.location_of(id)));
    }

    if (rhs_is_packed_field(resolving_, ref.rhs)) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "cannot take a reference to a field of a 'packed struct'; copy it into a local first",
            error::ILLEGAL_PACKED_FIELD_ADDRESS,
            resolving_.ast.location_of(id)));
    }
    if (rhs_is_vector_lane(resolving_, ref.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "cannot take a reference to a vector lane; read it into a local first",
                             error::ILLEGAL_VECTOR_LANE_ADDRESS,
                             resolving_.ast.location_of(id)));
    }

    // References already behave like values
    if (rhs_type.get_kind() == type_kind::REFERENCE) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take a reference to an already-reference-typed value; pass it "
                             "directly to alias the same referent",
                             error::ILLEGAL_REFERENCE_TO_REFERENCE,
                             resolving_.ast.location_of(id)));
    }

    if (auto poisoned{reject_unrooted_poly(id, ref.rhs)}) { return last_type_.emplace(*poisoned); }

    auto& referent{denoted_type(rhs_type)};
    auto& new_type{ctx_.get_reference(ref_addr_of_is_mutable(id), referent)};
    new_type.resolve<types::reference>(referent);

    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::address_of_expr& adr_of) -> void {
    PROFILE_FUNCTION();

    // `^.{a, b, c}` against a slice type with `const` elements resolves as `[N]T` and yields the
    // slice directly. The emitter decays the backing array into it
    if (ref_addr_of_is_mutable(id) != types::mut::MUTABLE) {
        if (const auto init{resolving_.ast.get_as_opt<ast::initializer_expr>(adr_of.rhs)};
            init && !init->object_type) {
            if (const auto implicit_type{implicit_type_stack_.peek()}) {
                if (const auto slice_data{implicit_type->get_data().as_opt<types::slice>()};
                    slice_data && slice_data->underlying.is_constant()) {
                    auto& elem_type{slice_data->underlying};
                    auto& array_type{ctx_.get_array(types::mut::CONSTANT,
                                                    slice_data->null_terminated,
                                                    init->initializers.size(),
                                                    elem_type)};
                    auto& slice_type{*implicit_type};
                    {
                        const structural_guard g{implicit_type_stack_, array_type};
                        TRY_RESOLVE(adr_of.rhs);
                    }
                    if (last_type_->is_poison()) {
                        return last_type_.emplace(ctx_.poison_node(resolving_, id));
                    }
                    resolving_.set_sema_type(id, slice_type);
                    return last_type_.emplace(slice_type);
                }
            }
        }
    }

    {
        const mutating_context_guard g{in_mutating_context_,
                                       ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
        const mutating_context_guard dyn_g{dyn_unsized_ok_, adr_of.rhs.is<ast::type_expr>()};
        TRY_RESOLVE(adr_of.rhs);
    }
    auto& rhs_type{*last_type_.take()};
    if (rhs_type.get_kind() == type_kind::ENUM_LITERAL) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_, id, enum_literal_has_no_address(resolving_.ast.location_of(id))));
    }

    if (names_comptime_mut(adr_of.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take the address of a `comptime let mut`; it has no storage",
                             error::COMPTIME_MUT_ADDRESS_OF,
                             resolving_.ast.location_of(id)));
    }

    // Semantic safeguard: disallow taking a mutable pointer to an rvalue/temporary
    // (e.g. `^mut 42`), which would immediately become a dangling pointer.
    const bool is_mut{ref_addr_of_is_mutable(id) == types::mut::MUTABLE};
    if (is_mut && !is_type_denoting_expr(ctx_, resolving_, adr_of.rhs, &table_stack_) &&
        !is_lvalue_expression(resolving_, adr_of.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "Cannot take a mutable pointer to a temporary value",
                             error::ILLEGAL_RVALUE_CAPTURE,
                             resolving_.ast.location_of(id)));
    }

    if (rhs_is_packed_field(resolving_, adr_of.rhs)) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "cannot take the address of a field of a 'packed struct'; copy it into a local first",
            error::ILLEGAL_PACKED_FIELD_ADDRESS,
            resolving_.ast.location_of(id)));
    }
    if (rhs_is_vector_lane(resolving_, adr_of.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "cannot take the address of a vector lane; read it into a local first",
                             error::ILLEGAL_VECTOR_LANE_ADDRESS,
                             resolving_.ast.location_of(id)));
    }

    if (auto poisoned{reject_unrooted_poly(id, adr_of.rhs)}) {
        return last_type_.emplace(*poisoned);
    }

    // `^@TypeOf(x)` / `^fn(...): T` point at the type the operand denotes, never at a `type` value
    gsl::not_null<type*> pointee{&denoted_type(rhs_type)};
    if (const auto ref{rhs_type.get_data().as_opt<types::reference>()}) {
        pointee = &ref->underlying;
    }

    auto& new_type{ctx_.get_pointer(ref_addr_of_is_mutable(id), *pointee)};
    new_type.resolve_if<types::pointer>(*pointee);
    resolving_.set_sema_type(id, new_type);
    last_type_.emplace(new_type);
}

auto type_resolver::visit(ast::node_id id, const ast::comptime_expr& cx) -> void {
    PROFILE_FUNCTION();
    // A function literal is a compile-time value already, and folding it loses the function
    if (cx.rhs.is<ast::function_expr>()) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "Redundant 'comptime': a function literal is already known at compile time",
            error::COMPTIME_EVALUATION_FAILED,
            resolving_.ast.location_of(id)));
    }
    // The operand is compile-time evaluation. The emitter folds it per instantiation
    const comptime_evaluation_scope cx_scope{ctx_, true};
    TRY_RESOLVE(*cx.rhs);
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::dereference_expr& deref) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(deref.rhs);
    auto& rhs_type{*last_type_.take()};

    // Check for a pointer or reference and update to the underlying type to enforce dereference
    // semantics
    if (const auto pointer{rhs_type.get_data().as_opt<types::pointer>()}) {
        last_type_.emplace(pointer->underlying);
    } else if (const auto ref{rhs_type.get_data().as_opt<types::reference>()}) {
        last_type_.emplace(ref->underlying);
    } else if (const auto slice{rhs_type.get_data().as_opt<types::slice>()}) {
        // Dereferencing a slice copies its elements out into an array of the same length
        const auto len{known_length(deref.rhs)};
        if (!len) {
            return last_type_.emplace(ctx_.poison_node(
                resolving_,
                id,
                "Cannot dereference a slice whose length is not known at compile time; slice it "
                "with constant bounds first (e.g. `s[i..][0..n]`)",
                error::UNKNOWN_SLICE_LENGTH,
                resolving_.ast.location_of(id)));
        }
        last_type_.emplace(
            ctx_.get_array(container_element_mutability(rhs_type), false, *len, slice->underlying));
    } else {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("Cannot dereference non-pointer expression; found '{}'",
                                         ctx_.type_display_name(rhs_type)),
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id)));
    }
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::unary_expr& node) -> void {
    PROFILE_FUNCTION();
    {
        const bool negates_literal{(id.get_token_type() == syntax::token_type_t::MINUS ||
                                    id.get_token_type() == syntax::token_type_t::MINUS_PERCENT) &&
                                   resolving_.ast[node.rhs].is<ast::int_literal_expr>()};
        const mutating_context_guard g{negated_literal_, negates_literal};
        TRY_RESOLVE(node.rhs);
    }
    // No unary operator takes a type (`-i64`). The emitter would have no value to apply it to
    if (const auto& operand{*last_type_};
        !operand.is_poison() && decl_value_denotes_type(node.rhs)) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             fmt::format("operator {} expects a value, but found the type '{}'",
                                         syntax::token_type::describe(id.get_token_type()),
                                         ctx_.type_display_name(denoted_type(operand))),
                             error::TYPE_USED_AS_VALUE,
                             resolving_.ast.location_of(node.rhs)));
    }
    if (id.get_token_type() == syntax::token_type_t::BANG) {
        // `!` on a `@Vector(N, bool)` negates each lane
        if (!is_vector(*last_type_)) {
            last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::BOOL));
        }
    } else if (id.get_token_type() == syntax::token_type_t::PLUS) {
        // Unary '+' is an identity that no GIR instruction checks, so validate it here
        auto&      operand_type{*last_type_};
        const auto kind{lane_type_of(operand_type).get_kind()};
        const bool ok{is_numeric(kind) || kind == type_kind::COMPTIME_INT ||
                      kind == type_kind::COMPTIME_FLOAT};
        if (!operand_type.is_poison() && !ok) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("unary '+' expects a numeric operand; found '{}'",
                                             ctx_.type_display_name(operand_type)),
                                 error::OPERATOR_TYPE_MISMATCH,
                                 resolving_.ast.location_of(id)));
        }
    } else if (id.get_token_type() == syntax::token_type_t::MINUS_PERCENT) {
        // Unlike plain unary '-', wrapping negation is well-defined on unsigned integers too
        auto&      operand_type{*last_type_};
        const auto kind{lane_type_of(operand_type).get_kind()};
        const bool ok{kind == type_kind::COMPTIME_INT || is_integer(kind)};
        if (!operand_type.is_poison() && !ok) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("operator '-%' expects an integer operand; found '{}'",
                                             ctx_.type_display_name(operand_type)),
                                 error::OPERATOR_TYPE_MISMATCH,
                                 resolving_.ast.location_of(id)));
        }
    }
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::unwrap_expr& unwrap) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(unwrap.operand);
    auto& operand_type{*last_type_.take()};

    const bool is_question{id.get_token_type() == syntax::token_type_t::QUESTION};
    const auto loc{resolving_.ast.location_of(id)};

    const auto nominal_shape{unwrap_shape_of(ctx_, operand_type)};

    if (!nominal_shape) {
        std::string note;
        if (const auto ud = operand_type.get_data().as_opt<types::union_t>()) {
            if (!ud->is_untagged && ud->fields.size() == 2) {
                note = fmt::format("; add 'impl builtin.Unwrappable for {}'",
                                   ctx_.type_display_name(operand_type));
            }
        }
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format(
                "the postfix '{}' operator expects a type implementing 'builtin.Unwrappable'; "
                "'{}' does not implement it{}",
                is_question ? "?" : "!",
                ctx_.type_display_name(operand_type),
                note),
            error::UNWRAP_ON_NON_RESULT,
            loc));
    }

    gsl::not_null<type*> payload_type{nominal_shape->output_type};

    // `expr!` just projects the success payload with lowering handling the discriminant check
    if (!is_question) {
        resolving_.set_sema_type(id, *payload_type);
        return last_type_.emplace(*payload_type);
    }

    // A body re-typed by a dedicated instantiation resolver has no `open_function_nodes_` entry
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
        // The `return_tracker` carrying the concrete return type can be used as a fallback
        ret_type.emplace(*return_trackers_.back().expected_type);
    } else if (return_trackers_.empty()) {
        return last_type_.emplace(
            ctx_.poison_node(resolving_,
                             id,
                             "the '?' operator can only be used inside a function",
                             error::UNWRAP_OUTSIDE_FUNCTION,
                             loc));
    }

    if (!ret_type) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            "the '?' operator requires the enclosing function's return type to be written "
            "explicitly rather than inferred with 'auto'",
            error::UNWRAP_RETURN_TYPE_MISMATCH,
            loc));
    }

    const auto nominal_rewrap{rewrap_shape_of(ctx_, *ret_type)};
    if (!nominal_rewrap) {
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("the '?' operator propagates a '{}' residual ('{}') but '{}' "
                        "does not implement 'builtin.Rewrappable'",
                        ctx_.type_display_name(operand_type),
                        ctx_.type_display_name(*nominal_shape->residual_type),
                        ctx_.type_display_name(*ret_type)),
            error::UNWRAP_RETURN_TYPE_MISMATCH,
            loc));
    }

    const auto& res_ty{*nominal_shape->residual_type};
    const auto& from_ty{*nominal_rewrap->from_type};
    const bool  same{is_same_unqualified(res_ty, from_ty)};
    const bool  assignable{is_assignable(res_ty, from_ty)};
    const bool  widenable{is_implicit_widenable(res_ty, from_ty)};
    if (!same && !assignable && !widenable) {
        const auto  ptr_bits{codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits};
        const auto  reason{cast_rejection_reason(res_ty, from_ty, ptr_bits, ctx_.user_type_names)};
        std::string reason_suffix;
        if (reason) { reason_suffix = fmt::format(" ({})", *reason); }
        return last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format(
                "the '?' operator propagates a '{}' residual ('{}') but '{}' is not "
                "rebuildable from '{}'{}; implement 'builtin.Rewrappable for {}' with From = '{}'",
                ctx_.type_display_name(operand_type),
                ctx_.type_display_name(res_ty),
                ctx_.type_display_name(*ret_type),
                ctx_.type_display_name(res_ty),
                reason_suffix,
                ctx_.type_display_name(*ret_type),
                ctx_.type_display_name(res_ty)),
            error::UNWRAP_RETURN_TYPE_MISMATCH,
            loc));
    }

    resolving_.set_sema_type(id, *payload_type);
    last_type_.emplace(*payload_type);
}

// String literals are just constant arrays of bytes
auto type_resolver::visit(ast::node_id id, const ast::string_expr& string) -> void {
    PROFILE_FUNCTION();

    // A string literal is a sentinel-terminated array: its `.len` is the character count, and
    // codegen sizes the storage one element larger to hold the implicit `\0`
    auto& type{
        ctx_.get_array(types::mut::CONSTANT, true, string.value.size(), ctx_.get_int(8, false))};

    // String literals with the same size will always have the same type
    resolving_.set_sema_type(id, type);
    last_type_.emplace(type);
}

#define MAKE_PRIMITIVE_RESOLVER(NodeType, kind)                                \
    auto type_resolver::visit(ast::node_id id, const ast::NodeType&) -> void { \
        PROFILE_FUNCTION();                                                    \
        last_type_.emplace(ctx_.get_builtin_resolved_type(type_kind::kind));   \
        last_type_->resolve_if<types::builtin_type>();                         \
        resolving_.set_sema_type(id, *last_type_);                             \
    }

auto type_resolver::visit(ast::node_id id, const ast::int_literal_expr& expr) -> void {
    PROFILE_FUNCTION();
    stdx::option<type&> resolved;
    if (expr.is_size) {
        resolved =
            ctx_.get_builtin_resolved_type(expr.is_signed ? type_kind::ISIZE : type_kind::USIZE);
    } else if (expr.width != 0) {
        resolved = ctx_.get_int(expr.width, expr.is_signed);
    }
    // A suffix names the literal's type, which must hold it
    if (resolved) {
        const auto ptr_bits{codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits};
        const auto value{negated_literal_ ? -expr.exact_value() : expr.exact_value()};
        if (!comptime_int_fits(value, *resolved, ptr_bits)) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 fmt::format("integer literal {} is out of range for type '{}'",
                                             expr.exact_value(),
                                             ctx_.type_display_name(*resolved)),
                                 error::LITERAL_OUT_OF_RANGE,
                                 resolving_.ast.location_of(id)));
        }
    } else {
        // An unsuffixed integer literal is `comptime_int` and coerces freely
        resolved = expr.is_char ? ctx_.get_char_constant()
                                : ctx_.get_builtin_resolved_type(type_kind::COMPTIME_INT);
        const auto implicit_type{implicit_type_stack_.peek()};
        if (implicit_type && implicit_type->get_kind() == type_kind::COMPTIME_FLOAT) {
            // Folding must see a float value, not an integer that merely coerces
            resolved = ctx_.get_builtin_resolved_type(type_kind::COMPTIME_FLOAT);
        } else if (implicit_type &&
                   (is_integer(implicit_type->get_kind()) || is_float(implicit_type->get_kind()))) {
            // Adopt the concrete context only when the literal's magnitude fits it
            const auto ptr_bits{
                codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits};
            const bool fits{
                expr.is_big
                    ? comptime_int_fits(expr.exact_value(), *implicit_type, ptr_bits)
                    : comptime_int_fits(static_cast<i128>(expr.value), *implicit_type, ptr_bits)};
            if ((is_float(implicit_type->get_kind()) && !expr.is_big) || fits) {
                resolved = *ctx_.pool.strip_volatile(*implicit_type);
            }
        }
    }
    if (const auto format{float_format_of(*resolved)}) {
        if (const auto problem{float_literal_range_problem(
                f128::from_uint(expr.value), f128::from_uint(expr.value, *format), *resolved)}) {
            return last_type_.emplace(float_literal_out_of_range(id, *problem));
        }
    }
    last_type_.emplace(*resolved);
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::visit(ast::node_id id, const ast::float_literal_expr& expr) -> void {
    PROFILE_FUNCTION();
    type_kind kind{type_kind::F64};
    switch (expr.width) {
    case 16:  kind = type_kind::F16; break;
    case 32:  kind = type_kind::F32; break;
    case 80:  kind = type_kind::F80; break;
    case 128: kind = type_kind::F128; break;
    default:  kind = type_kind::F64; break; // 0 (width-less) and 64
    }

    // An unsuffixed real literal is `comptime_float` and coerces freely
    gsl::not_null<type*> resolved{expr.width == 0
                                      ? &ctx_.get_builtin_resolved_type(type_kind::COMPTIME_FLOAT)
                                      : &ctx_.get_builtin_resolved_type(kind)};
    if (expr.width == 0) {
        if (const auto implicit_type{implicit_type_stack_.peek()};
            implicit_type && is_float(implicit_type->get_kind())) {
            resolved = ctx_.pool.strip_volatile(*implicit_type);
        }
    }
    if (const auto format{float_format_of(*resolved)}) {
        if (const auto problem{
                float_literal_range_problem(expr.value, expr.value_in(*format), *resolved)}) {
            return last_type_.emplace(float_literal_out_of_range(id, *problem));
        }
    }
    last_type_.emplace(*resolved);
    resolving_.set_sema_type(id, *last_type_);
}

auto type_resolver::float_literal_range_problem(f128 exact, f128 rounded, const type& target)
    -> stdx::option<std::string> {
    if (exact.is_finite() && rounded.is_infinite()) {
        return fmt::format("literal is out of range for type '{}'", ctx_.type_display_name(target));
    }
    if (!exact.is_zero() && rounded.is_zero()) {
        return fmt::format("literal is too small for type '{}' and would round to zero",
                           ctx_.type_display_name(target));
    }
    return stdx::none;
}

auto type_resolver::float_literal_out_of_range(ast::node_id id, std::string message) -> type& {
    return ctx_.poison_node(resolving_,
                            id,
                            std::move(message),
                            error::LITERAL_OUT_OF_RANGE,
                            resolving_.ast.location_of(id));
}

MAKE_PRIMITIVE_RESOLVER(bool_expr, BOOL)

MAKE_PRIMITIVE_RESOLVER(void_expr, VOID_)

MAKE_PRIMITIVE_RESOLVER(undefined_expr, UNDEFINED)

MAKE_PRIMITIVE_RESOLVER(nullptr_expr, NULLPTR)

MAKE_PRIMITIVE_RESOLVER(unreachable_expr, NORETURN)

auto type_resolver::visit(ast::node_id id, const ast::type_expr& node) -> void {
    PROFILE_FUNCTION();
    TRY_RESOLVE(node.type);
    resolving_.set_sema_type(id, *last_type_);
}

// A `let mut`, or a `const` that folds to something other than a type, cannot annotate a type
// Folds `expr` purely to inspect it. A failed attempt's diagnostics are not the user's error
auto type_resolver::probe_fold(ast::expr_handle expr) const -> stdx::option<gir::const_value> {
    const auto      diags_before{ctx_.diags.size()};
    gir::const_eval evaluator{ctx_, resolving_};
    auto            folded{evaluator.try_eval(expr)};
    if (ctx_.diags.size() > diags_before) {
        DISCARD(ctx_.diags.split_off(diags_before));
        return stdx::none;
    }
    return folded;
}

auto type_resolver::lhs_decides_logical_op(syntax::token_type_t op, ast::expr_handle lhs) const
    -> bool {
    if (op != syntax::token_type_t::BOOLEAN_AND && op != syntax::token_type_t::BOOLEAN_OR) {
        return false;
    }
    const auto folded{probe_fold(lhs)};
    return folded && folded->is<bool>() &&
           folded->as<bool>() == (op == syntax::token_type_t::BOOLEAN_OR);
}

auto type_resolver::operand_nature(ast::expr_handle expr) const -> operand_nature_t {
    const auto t{resolving_.get_sema_type_opt(expr)};
    if (!t || t->is_poison()) { return operand_nature_t::UNKNOWN; }
    if (ast::node_id{expr}.get_token_type() == syntax::token_type_t::TYPE_TYPE) {
        return operand_nature_t::TYPE;
    }
    // A template placeholder or unfolded `[n]T` is `type`-kinded until instantiated
    if (t->get_kind() == type_kind::TYPE && !t->get_data().is<types::meta_type>()) {
        return operand_nature_t::UNKNOWN;
    }
    // `^X` / `&X` is a pointer or reference type exactly when `X` is a type
    if (const auto addr{resolving_.ast.get_as_opt<ast::address_of_expr>(expr)}) {
        return operand_nature(addr->rhs);
    }
    if (const auto ref{resolving_.ast.get_as_opt<ast::reference_expr>(expr)}) {
        return operand_nature(ref->rhs);
    }
    if (decl_value_denotes_type(expr)) { return operand_nature_t::TYPE; }
    // A bound `T: type` parameter or `@TypeOf(...)` only reveals itself by folding
    const auto folded{probe_fold(expr)};
    if (folded && folded->is<stdx::option<sema::type&>>()) { return operand_nature_t::TYPE; }
    return operand_nature_t::VALUE;
}

auto type_resolver::call_arg_denotes_type(const ast::call_expr::argument& arg) const -> bool {
    if (arg.is<ast::explicit_type_id>()) { return true; }
    const auto expr{arg.as_opt<ast::expr_handle>()};
    return expr && operand_nature(*expr) == operand_nature_t::TYPE;
}

auto type_resolver::names_a_value(const symbol& sym, usize table_idx) -> bool {
    // Prelude symbols' nodes index the builtin module's AST, not this one
    if (ctx_.prelude_index && table_idx == *ctx_.prelude_index) { return false; }
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    if (!node) { return false; }
    const auto decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*node)};
    if (!decl) { return false; }
    if (decl->has_modifier(ast::decl_modifiers::MUT)) { return true; }
    if (!decl->value) { return false; }

    const auto folded{probe_fold(*decl->value)};
    return folded && !folded->is_poison() && !folded->is<stdx::option<sema::type&>>();
}

auto type_resolver::comptime_type_var_value(const symbol& sym) -> stdx::option<type&> {
    if (!sym.has_kind() || sym.get_kind() != symbol_kind::TYPE) { return stdx::none; }
    const auto node{sym.get_data().as_opt<symbols::node_t>()};
    const auto decl{node ? resolving_.ast.get_as_opt<ast::decl_stmt>(*node) : stdx::none};
    if (!decl || !decl->has_modifier(ast::decl_modifiers::COMPTIME) ||
        !decl->has_modifier(ast::decl_modifiers::MUT)) {
        return stdx::none;
    }
    const auto& name{resolving_.ast.get_as<ast::identifier_expr>(decl->name).name};
    if (name != sym.get_name()) { return stdx::none; }

    // Its value here is whatever the statements before this point left it holding
    const comptime_frame_guard sim_guard{ctx_.comptime_binding_frames, make_simulated_frame()};
    const auto                 current{ctx_.lookup_comptime_binding(name)};
    const auto                 held{current ? current->as_opt<stdx::option<type&>>() : stdx::none};
    if (held && *held) { return **held; }
    return stdx::none;
}

} // namespace ghoti::sema
