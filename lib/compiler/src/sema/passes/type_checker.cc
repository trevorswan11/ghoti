#include "compiler/sema/passes/type_checker.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::sema {

type_checker::type_checker(gir::module& gir_mod, context& ctx) noexcept
    : gir_mod_{gir_mod}, ctx_{ctx},
      target_ptr_bits_{codegen::target_facts::resolve(ctx.target_opts.triple_str).ptr_bits} {}

auto type_checker::check_types(gir::module& gir_mod, mod::module& ast_mod, context& ctx)
    -> mod::module_state {
    PROFILE_FUNCTION();
    type_checker checker{gir_mod, ctx};

    for (const auto& fn : gir_mod.get_functions()) { checker.check_function(*fn); }
    if (!ctx.diags.empty() || ast_mod.is_poisoned()) {
        return ast_mod.error_out(std::move(ctx.diags), mod::module_state::POISONED_TYPE_RESOLVED);
    }
    return ast_mod.state;
}

auto type_checker::format_store_mismatch(const type& val_t, const type& dest_t) const
    -> std::string {
    if (const auto reason{cast_rejection_reason(val_t, dest_t, target_ptr_bits_)}) {
        return fmt::format("Type mismatch in store: cannot assign '{}' to '{}' ({})",
                           type_kind_display_name(val_t),
                           type_kind_display_name(dest_t),
                           *reason);
    }
    return fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                       type_kind_display_name(val_t),
                       type_kind_display_name(dest_t));
}

auto type_checker::format_arg_mismatch(usize                          arg_idx,
                                       const type&                    arg_t,
                                       const type&                    param_t,
                                       stdx::option<std::string_view> callee) const -> std::string {
    const auto reason{cast_rejection_reason(arg_t, param_t, target_ptr_bits_)};
    if (callee) {
        if (reason) {
            return fmt::format(
                "Argument {} of type '{}' is not assignable to parameter type '{}' in call to '{}' "
                "({})",
                arg_idx,
                type_kind_display_name(arg_t),
                type_kind_display_name(param_t),
                *callee,
                *reason);
        }
        return fmt::format(
            "Argument {} of type '{}' is not assignable to parameter type '{}' in call to '{}'",
            arg_idx,
            type_kind_display_name(arg_t),
            type_kind_display_name(param_t),
            *callee);
    }
    if (reason) {
        return fmt::format(
            "Argument {} of type '{}' is not assignable to parameter type '{}' in indirect call "
            "({})",
            arg_idx,
            type_kind_display_name(arg_t),
            type_kind_display_name(param_t),
            *reason);
    }
    return fmt::format(
        "Argument {} of type '{}' is not assignable to parameter type '{}' in indirect call",
        arg_idx,
        type_kind_display_name(arg_t),
        type_kind_display_name(param_t));
}

auto type_checker::format_return_mismatch(const type& ret_t, const type& expected_t) const
    -> std::string {
    if (const auto reason{cast_rejection_reason(ret_t, expected_t, target_ptr_bits_)}) {
        return fmt::format(
            "Return value of type '{}' is not assignable to function return type '{}' ({})",
            type_kind_display_name(ret_t),
            type_kind_display_name(expected_t),
            *reason);
    }
    return fmt::format("Return value of type '{}' is not assignable to function return type '{}'",
                       type_kind_display_name(ret_t),
                       type_kind_display_name(expected_t));
}

namespace {

auto folded_int(const gir::value& v) noexcept -> stdx::option<i128> {
    if (const auto x{v.as_opt<i64>()}) { return static_cast<i128>(*x); }
    if (const auto x{v.as_opt<i128>()}) { return *x; }
    if (const auto x{v.as_opt<u64>()}) { return static_cast<i128>(*x); }
    if (const auto x{v.as_opt<u128>()}) { return static_cast<i128>(*x); }
    return stdx::none;
}

} // namespace

auto type_checker::is_value_assignable(const gir::value&             val,
                                       const type&                   val_t,
                                       const type&                   dest_t,
                                       stdx::option<source_location> loc) -> bool {
    if (is_assignable(val_t, dest_t)) { return true; }
    if (is_integer(val_t.get_kind()) && is_integer(dest_t.get_kind())) {
        if (const auto folded{folded_int(val)}) {
            if (constexpr_int_fits(*folded, dest_t, target_ptr_bits_)) { return true; }
            emit_diagnostic(fmt::format("integer value {} is out of range for type '{}'",
                                        *folded,
                                        type_kind_display_name(dest_t)),
                            error::LITERAL_OUT_OF_RANGE,
                            loc);
            return true;
        }
    }
    return false;
}

auto type_checker::emit_diagnostic(std::string_view              message,
                                   error                         err,
                                   stdx::option<source_location> loc) -> void {
    ctx_.diags.emplace_back(std::string{message}, err, loc);
}

auto type_checker::get_operand_type(const gir::value& val) -> stdx::option<type&> {
    const auto concrete{[&](type& t) -> type& {
        if (t.get_kind() == type_kind::CONSTEXPR_INT) { return ctx_.get_int(32, true); }
        if (t.get_kind() == type_kind::CONSTEXPR_FLOAT) {
            return ctx_.get_builtin_resolved_type(type_kind::F64);
        }
        return t;
    }};
    if (val.type) { return concrete(*val.type); }
    if (const auto lid{val.data.as_opt<gir::local_id>()}) {
        if (auto it{locals_.find(*lid)}; it != locals_.end()) { return concrete(*it->second.type); }
    }
    return stdx::none;
}

auto type_checker::find_function(std::string_view name) const -> stdx::option<gir::function&> {
    for (auto* f : gir_mod_.get_functions()) {
        if (f->get_name() == name) { return *f; }
    }
    return stdx::none;
}

auto type_checker::check_function(gir::function& fn) -> void {
    PROFILE_FUNCTION();
    locals_.clear();

    for (const auto& param : fn.get_params()) {
        locals_.emplace(param->id,
                        local_info{.type = &param->type, .is_alloca = false, .is_const = false});
    }

    const auto&       segments{fn.get_segments()};
    std::vector<bool> reachable(segments.size(), false);
    if (!segments.empty()) {
        std::vector<gir::segment_id> worklist;
        reachable[0] = true;
        worklist.emplace_back(segments[0]->get_id());

        while (!worklist.empty()) {
            const auto curr_id{worklist.back()};
            worklist.pop_back();

            const auto curr_seg_opt{fn.get_segment_opt(curr_id)};
            if (!curr_seg_opt) { continue; }

            for (const auto* inst : (*curr_seg_opt)->get_instructions()) {
                if (inst->target_segment) {
                    const auto target_idx{std::to_underlying(*inst->target_segment)};
                    if (target_idx < segments.size() && !reachable[target_idx]) {
                        reachable[target_idx] = true;
                        worklist.emplace_back(*inst->target_segment);
                    }
                }

                if (inst->true_segment) {
                    const auto target_idx{std::to_underlying(*inst->true_segment)};
                    if (target_idx < segments.size() && !reachable[target_idx]) {
                        reachable[target_idx] = true;
                        worklist.emplace_back(*inst->true_segment);
                    }
                }

                if (inst->false_segment) {
                    const auto target_idx{std::to_underlying(*inst->false_segment)};
                    if (target_idx < segments.size() && !reachable[target_idx]) {
                        reachable[target_idx] = true;
                        worklist.emplace_back(*inst->false_segment);
                    }
                }
            }
        }
    }

    for (const auto& seg : segments) {
        const auto idx{std::to_underlying(seg->get_id())};
        if (idx < reachable.size() && !reachable[idx]) { continue; }
        check_segment(fn, *seg);
    }
}

auto type_checker::check_segment(gir::function& fn, gir::segment& seg) -> void {
    PROFILE_FUNCTION();
    for (const auto* inst : seg.get_instructions()) { check_instruction(fn, *inst); }
}

auto type_checker::check_instruction(gir::function& fn, const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();

    switch (inst.kind) {
    case gir::instruction_kind::ALLOCA: {
        if (inst.type && inst.type->get_kind() == type_kind::OPAQUE) {
            emit_diagnostic("Cannot allocate variable of opaque type",
                            error::ILLEGAL_OPAQUE_TYPE,
                            inst.location);
        }
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = true,
                                         .is_const  = inst.is_const,
                                     });
        }
        break;
    }
    case gir::instruction_kind::LOAD: {
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::STORE: {
        check_store(inst);
        break;
    }
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL:
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_numeric(lhs_t->get_kind()) || !is_numeric(rhs_t->get_kind()) ||
                    !is_same_unqualified(*lhs_t, *rhs_t)) {
                    // Check if one is integer and one is pointer
                    const auto lhs_ptr_rhs_int{lhs_t->get_kind() == type_kind::POINTER &&
                                               is_integer(rhs_t->get_kind())};
                    const auto rhs_ptr_lhs_int{rhs_t->get_kind() == type_kind::POINTER &&
                                               is_integer(lhs_t->get_kind())};
                    const bool ptr_arith{
                        (inst.kind == gir::instruction_kind::ADD ||
                         inst.kind == gir::instruction_kind::SUB) &&
                        (lhs_ptr_rhs_int ||
                         (inst.kind == gir::instruction_kind::ADD && rhs_ptr_lhs_int))};
                    if (!ptr_arith) {
                        emit_diagnostic(
                            fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                        gir::instruction_kind_name(inst.kind),
                                        type_kind_display_name(*lhs_t),
                                        type_kind_display_name(*rhs_t)),
                            error::OPERATOR_TYPE_MISMATCH,
                            inst.location);
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::AND:
    case gir::instruction_kind::OR:  {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                const bool both_bool{lhs_t->get_kind() == type_kind::BOOL &&
                                     rhs_t->get_kind() == type_kind::BOOL};
                const bool both_same_int{is_integer(lhs_t->get_kind()) &&
                                         is_integer(rhs_t->get_kind()) &&
                                         is_same_unqualified(*lhs_t, *rhs_t)};
                if (!both_bool && !both_same_int) {
                    emit_diagnostic(
                        fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                    gir::instruction_kind_name(inst.kind),
                                    type_kind_display_name(*lhs_t),
                                    type_kind_display_name(*rhs_t)),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::XOR:
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_integer(lhs_t->get_kind()) || !is_integer(rhs_t->get_kind()) ||
                    !is_same_unqualified(*lhs_t, *rhs_t)) {
                    emit_diagnostic(
                        fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                    gir::instruction_kind_name(inst.kind),
                                    type_kind_display_name(*lhs_t),
                                    type_kind_display_name(*rhs_t)),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::EQ:
    case gir::instruction_kind::NE: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                // Structurals lower to LLVM structs/arrays, which have no '=='/'!=' semantics.
                if (is_structural(lhs_t->get_kind()) || is_structural(rhs_t->get_kind())) {
                    emit_diagnostic(
                        fmt::format("Comparison operator cannot be applied to aggregate types "
                                    "'{}' and '{}'",
                                    type_kind_display_name(*lhs_t),
                                    type_kind_display_name(*rhs_t)),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                } else if (!is_assignable(*lhs_t, *rhs_t) && !is_assignable(*rhs_t, *lhs_t)) {
                    emit_diagnostic(
                        fmt::format("Comparison operator cannot be applied to incompatible types "
                                    "'{}' and '{}'",
                                    type_kind_display_name(*lhs_t),
                                    type_kind_display_name(*rhs_t)),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::LT:
    case gir::instruction_kind::LE:
    case gir::instruction_kind::GT:
    case gir::instruction_kind::GE: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_numeric(lhs_t->get_kind()) || !is_numeric(rhs_t->get_kind()) ||
                    (!is_assignable(*lhs_t, *rhs_t) && !is_assignable(*rhs_t, *lhs_t))) {
                    emit_diagnostic(
                        fmt::format("Relational operator cannot be applied to non-numeric or "
                                    "incompatible types '{}' and '{}'",
                                    type_kind_display_name(*lhs_t),
                                    type_kind_display_name(*rhs_t)),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::NEG: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (!is_signed_integer(*op_t) && !is_float(op_t->get_kind())) {
                    emit_diagnostic("Unary negation '-' requires a signed integer or float operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::NOT: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (op_t->get_kind() != type_kind::BOOL) {
                    emit_diagnostic("Logical negation '!' requires a boolean operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::BITNOT: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (!is_integer(op_t->get_kind())) {
                    emit_diagnostic("Bitwise negation '~' requires an integer operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::RET: {
        const auto& fn_type{fn.get_type()};
        const auto  fn_data{fn_type.get_data().as_opt<types::function>()};
        if (!fn_data) { break; }

        auto& expected_ret_t{fn_data->return_type};
        if (expected_ret_t.get_kind() == type_kind::VOID_) {
            if (!inst.operands.empty()) {
                // Returning from the void is fine (jason turner moment)
                const auto ret_t{get_operand_type(inst.operands[0])};
                if (ret_t && ret_t->get_kind() != type_kind::VOID_ && !ret_t->is_poison()) {
                    emit_diagnostic("Cannot return a value from a function returning void",
                                    error::RETURN_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        } else {
            if (inst.operands.empty() || inst.operands[0].data.is<gir::void_val>()) {
                emit_diagnostic(fmt::format("Empty return in function expecting return type '{}'",
                                            type_kind_display_name(expected_ret_t)),
                                error::RETURN_TYPE_MISMATCH,
                                inst.location);
            } else if (inst.operands[0].data.is<gir::undefined_val>()) {
                const auto ret_t{get_operand_type(inst.operands[0])};
                if (!ret_t || ret_t->get_kind() != type_kind::NORETURN) {
                    emit_diagnostic(fmt::format("Function expecting return type '{}' does not "
                                                "return a value on all code paths",
                                                type_kind_display_name(expected_ret_t)),
                                    error::RETURN_TYPE_MISMATCH,
                                    inst.location);
                }
            } else {
                const auto ret_t{get_operand_type(inst.operands[0])};
                if (ret_t && !ret_t->is_poison() &&
                    !is_value_assignable(inst.operands[0], *ret_t, expected_ret_t, inst.location)) {
                    emit_diagnostic(format_return_mismatch(*ret_t, expected_ret_t),
                                    error::RETURN_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }
        break;
    }
    case gir::instruction_kind::CALL:
    case gir::instruction_kind::BUILTIN_CALL: {
        if (inst.callee_name) {
            if (const auto callee{find_function(*inst.callee_name)}) {
                const auto& params{callee->get_params()};
                if (callee->get_is_variadic()) {
                    if (inst.operands.size() < params.size()) {
                        emit_diagnostic(
                            fmt::format(
                                "Function '{}' expects at least {} arguments but {} were provided",
                                *inst.callee_name,
                                params.size(),
                                inst.operands.size()),
                            error::ARITY_MISMATCH,
                            inst.location);
                    } else {
                        for (usize i{0}; i < params.size(); ++i) {
                            const auto arg_t{get_operand_type(inst.operands[i])};
                            if (arg_t && !arg_t->is_poison() &&
                                arg_t->get_kind() != sema::type_kind::TYPE &&
                                params[i]->type.get_kind() != sema::type_kind::TYPE &&
                                !is_value_assignable(
                                    inst.operands[i], *arg_t, params[i]->type, inst.location)) {
                                emit_diagnostic(
                                    format_arg_mismatch(
                                        i + 1, *arg_t, params[i]->type, *inst.callee_name),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                            }
                        }
                    }
                } else if (params.size() != inst.operands.size()) {
                    emit_diagnostic(
                        fmt::format("Function '{}' expects {} arguments but {} were provided",
                                    *inst.callee_name,
                                    params.size(),
                                    inst.operands.size()),
                        error::ARITY_MISMATCH,
                        inst.location);
                } else {
                    for (usize i{0}; i < params.size(); ++i) {
                        const auto arg_t{get_operand_type(inst.operands[i])};
                        if (arg_t && !arg_t->is_poison() &&
                            arg_t->get_kind() != sema::type_kind::TYPE &&
                            params[i]->type.get_kind() != sema::type_kind::TYPE &&
                            !is_value_assignable(
                                inst.operands[i], *arg_t, params[i]->type, inst.location)) {
                            emit_diagnostic(format_arg_mismatch(
                                                i + 1, *arg_t, params[i]->type, *inst.callee_name),
                                            error::TYPE_MISMATCH,
                                            inst.location);
                        }
                    }
                }
            }
        } else if (!inst.operands.empty()) {
            const auto callee_t{get_operand_type(inst.operands[0])};
            if (callee_t && !callee_t->is_poison()) {
                stdx::option<const types::function&> fn_data;
                if (const auto ptr_data{callee_t->get_data().as_opt<types::pointer>()}) {
                    fn_data = ptr_data->underlying.get_data().as_opt<types::function>();
                } else {
                    fn_data = callee_t->get_data().as_opt<types::function>();
                }

                if (fn_data) {
                    const auto expected_args{fn_data->params.size()};
                    const auto provided_args{inst.operands.size() - 1};

                    if (fn_data->is_variadic) {
                        if (provided_args < expected_args) {
                            emit_diagnostic(
                                fmt::format("Indirect function call expects at least {} arguments "
                                            "but {} were provided",
                                            expected_args,
                                            provided_args),
                                error::ARITY_MISMATCH,
                                inst.location);
                        } else {
                            for (usize i{0}; i < expected_args; ++i) {
                                const auto arg_t{get_operand_type(inst.operands[i + 1])};
                                if (arg_t && !arg_t->is_poison() &&
                                    arg_t->get_kind() != sema::type_kind::TYPE &&
                                    !is_value_assignable(inst.operands[i + 1],
                                                         *arg_t,
                                                         *fn_data->params[i],
                                                         inst.location)) {
                                    emit_diagnostic(
                                        format_arg_mismatch(
                                            i + 1, *arg_t, *(fn_data->params[i]), stdx::none),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                                }
                            }
                        }
                    } else if (provided_args != expected_args) {
                        emit_diagnostic(
                            fmt::format("Indirect function call expects {} arguments but {} were "
                                        "provided",
                                        expected_args,
                                        provided_args),
                            error::ARITY_MISMATCH,
                            inst.location);
                    } else {
                        for (usize i{0}; i < expected_args; ++i) {
                            const auto arg_t{get_operand_type(inst.operands[i + 1])};
                            if (arg_t && !arg_t->is_poison() &&
                                arg_t->get_kind() != sema::type_kind::TYPE &&
                                !is_value_assignable(inst.operands[i + 1],
                                                     *arg_t,
                                                     *fn_data->params[i],
                                                     inst.location)) {
                                emit_diagnostic(
                                    format_arg_mismatch(
                                        i + 1, *arg_t, *(fn_data->params[i]), stdx::none),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                            }
                        }
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::PTR_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                if (src_t->get_kind() == type_kind::POINTER &&
                    dest_t->get_kind() == type_kind::POINTER) {
                    const auto src_ptr{src_t->get_data().as_opt<types::pointer>()};
                    const auto dest_ptr{dest_t->get_data().as_opt<types::pointer>()};
                    if (src_ptr && dest_ptr) {
                        if (src_t->is_constant() && !dest_t->is_constant()) {
                            emit_diagnostic(
                                "Cannot cast away const from pointer without @constCast",
                                error::ILLEGAL_CONST_CAST,
                                inst.location);
                        }
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::INT_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                const auto src_k{src_t->get_kind()};
                const auto dest_k{dest_t->get_kind()};
                const bool src_is_int{is_integer(src_k) || src_k == type_kind::CONSTEXPR_INT ||
                                      src_k == type_kind::BOOL};
                const bool dest_is_int{is_integer(dest_k)};
                if (!src_is_int || !dest_is_int) {
                    emit_diagnostic(fmt::format("Cannot @intCast type '{}' to '{}'",
                                                type_kind_display_name(*src_t),
                                                type_kind_display_name(*dest_t)),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::TRUNC_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                const auto src_k{src_t->get_kind()};
                const auto dest_k{dest_t->get_kind()};
                if (!is_integer(src_k) || !is_integer(dest_k)) {
                    emit_diagnostic(fmt::format("Cannot @truncate type '{}' to '{}'",
                                                type_kind_display_name(*src_t),
                                                type_kind_display_name(*dest_t)),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::WIDEN_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                const auto src_k{src_t->get_kind()};
                const auto dest_k{dest_t->get_kind()};
                const bool both_int{is_integer(src_k) && is_integer(dest_k)};
                const bool has_bool{src_k == type_kind::BOOL || dest_k == type_kind::BOOL};
                const bool is_enum_repr_cast{
                    (dest_k == type_kind::ENUM && sema::is_numeric(src_k)) ||
                    (src_k == type_kind::ENUM && sema::is_numeric(dest_k)) ||
                    (src_k == type_kind::ENUM && dest_k == type_kind::ENUM)};
                const bool is_float_cast{(sema::is_float(src_k) && sema::is_float(dest_k)) ||
                                         (sema::is_float(src_k) && is_integer(dest_k)) ||
                                         (is_integer(src_k) && sema::is_float(dest_k))};

                bool allowed{false};
                if (is_same_unqualified(*src_t, *dest_t)) {
                    allowed = true;
                } else if (has_bool) {
                    allowed = false;
                } else if (both_int) {
                    if (is_implicit_widenable(*src_t, *dest_t)) {
                        allowed = true;
                    } else if (const auto folded{folded_int(inst.operands[0])}) {
                        allowed = constexpr_int_fits(*folded, *dest_t, target_ptr_bits_);
                    }
                } else if (is_enum_repr_cast || is_float_cast) {
                    allowed = true;
                }

                if (!allowed) {
                    if (const auto reason{
                            cast_rejection_reason(*src_t, *dest_t, target_ptr_bits_)}) {
                        emit_diagnostic(fmt::format("Cannot cast type '{}' to '{}' ({})",
                                                    type_kind_display_name(*src_t),
                                                    type_kind_display_name(*dest_t),
                                                    *reason),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                    } else {
                        emit_diagnostic(fmt::format("Cannot cast type '{}' to '{}'",
                                                    type_kind_display_name(*src_t),
                                                    type_kind_display_name(*dest_t)),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::COND_GOTO:
        if (!inst.operands.empty()) {
            const auto cond_t{get_operand_type(inst.operands[0])};
            if (cond_t && !cond_t->is_poison() && cond_t->get_kind() != type_kind::BOOL) {
                emit_diagnostic("Conditional branch condition must be of type 'bool'",
                                error::TYPE_MISMATCH,
                                inst.location);
            }
        }
        break;
    case gir::instruction_kind::GET_ELEMENT_PTR:
        // The result type carries the container's own mutability for element const correctness
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = inst.type->is_constant(),
                                     });
        }
        break;
    case gir::instruction_kind::GLOBAL_ADDR:
        // Behaves like an alloca slot: `Type.X = v` / `g = v` stores validate against `is_const`.
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = true,
                                         .is_const  = inst.is_const,
                                     });
        }
        break;
    case gir::instruction_kind::ADDRESS_OF: {
        // Enforce `&mut`/`^mut` const correctness
        const auto result_mutable{inst.type && !inst.type->is_poison() &&
                                  (inst.type->get_kind() == type_kind::POINTER ||
                                   inst.type->get_kind() == type_kind::REFERENCE) &&
                                  !inst.type->is_constant()};
        if (result_mutable && !inst.operands.empty()) {
            if (const auto lid{inst.operands[0].as_opt<gir::local_id>()}) {
                if (const auto it{locals_.find(*lid)};
                    it != locals_.end() && it->second.is_const && it->second.is_alloca &&
                    it->second.type &&
                    it->second.type->get_kind() !=
                        type_kind::CLOSURE // closure's `const` binding only pins the binding, not
                                           // its captured env
                ) {
                    emit_diagnostic(
                        "Cannot take a mutable reference or pointer to a constant binding",
                        error::ASSIGNMENT_TO_CONST,
                        inst.location);
                }
            }
        }
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::DEREF:
    case gir::instruction_kind::INT_FROM_PTR:
    case gir::instruction_kind::PTR_FROM_INT:
    case gir::instruction_kind::BIT_CAST:
    case gir::instruction_kind::CONSTANT:
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = inst.kind == gir::instruction_kind::CONSTANT,
                                     });
        }
        break;
    case gir::instruction_kind::INLINE_ASM:
        // Operand well-formedness is enforced during type resolution
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    case gir::instruction_kind::GOTO:
    case gir::instruction_kind::UNREACHABLE: break;
    }
}

auto type_checker::check_store(const gir::instruction& inst) -> void {
    // A bit-packed struct slot is really a backing integer
    const auto packed_backing_store{[](const type* dest, const type* val) -> bool {
        if (!dest || !val || !is_integer(val->get_kind())) { return false; }
        if (const auto st{dest->get_data().as_opt<types::struct_t>()}) {
            return st->is_bit_packed();
        }
        if (const auto ut{dest->get_data().as_opt<types::union_t>()}) {
            return ut->is_bit_packed();
        }
        return false;
    }};

    if (inst.result && !inst.operands.empty()) {
        // Case A: Storing to a local_id alloca slot or param
        if (auto it{locals_.find(*inst.result)}; it != locals_.end()) {
            const auto val_t{get_operand_type(inst.operands[0])};
            if (it->second.is_alloca) {
                if (it->second.is_const && !inst.is_initializer) {
                    emit_diagnostic("Cannot assign to constant variable",
                                    error::ASSIGNMENT_TO_CONST,
                                    inst.location);
                    return;
                }

                if (val_t &&
                    !is_value_assignable(
                        inst.operands[0], *val_t, *it->second.type, inst.location) &&
                    !packed_backing_store(it->second.type, &*val_t)) {
                    emit_diagnostic(format_store_mismatch(*val_t, *it->second.type),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                }
                return;
            }

            if (it->second.type->get_kind() == type_kind::POINTER) {
                const auto ptr_data{it->second.type->get_data().as_opt<types::pointer>()};
                if (val_t && is_assignable(*val_t, *it->second.type)) {
                    // Storing pointer value into pointer variable or field: p = q
                    if (it->second.is_const && !inst.is_initializer) {
                        emit_diagnostic("Cannot assign to constant variable",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }
                    return;
                }
                if (ptr_data && val_t &&
                    is_value_assignable(
                        inst.operands[0], *val_t, ptr_data->underlying, inst.location)) {
                    // Storing through pointer: *p = val
                    if (it->second.type->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }
                    return;
                } else {
                    if (it->second.type->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                    } else {
                        if (val_t && ptr_data) {
                            emit_diagnostic(format_store_mismatch(*val_t, ptr_data->underlying),
                                            error::TYPE_MISMATCH,
                                            inst.location);
                        } else {
                            emit_diagnostic(
                                fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                                            val_t ? type_kind_display_name(*val_t) : "unknown",
                                            ptr_data ? type_kind_display_name(ptr_data->underlying)
                                                     : type_kind_display_name(*it->second.type)),
                                error::TYPE_MISMATCH,
                                inst.location);
                        }
                    }
                }
                return;
            }

            if (it->second.type->get_kind() == type_kind::REFERENCE) {
                const auto ref_data{it->second.type->get_data().as_opt<types::reference>()};
                if (ref_data) {
                    if (inst.is_initializer) {
                        if (val_t &&
                            !is_value_assignable(
                                inst.operands[0], *val_t, *it->second.type, inst.location)) {
                            emit_diagnostic(format_store_mismatch(*val_t, *it->second.type),
                                            error::TYPE_MISMATCH,
                                            inst.location);
                        }
                        return;
                    }

                    // References are never reseated unlike pointers
                    if (it->second.type->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through reference",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }
                    if (val_t &&
                        !is_value_assignable(
                            inst.operands[0], *val_t, ref_data->underlying, inst.location)) {
                        emit_diagnostic(format_store_mismatch(*val_t, ref_data->underlying),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                    }
                }
                return;
            }

            if (it->second.is_const && !inst.is_initializer) {
                emit_diagnostic("Cannot assign to an element of a non-mutable array or slice",
                                error::ASSIGNMENT_TO_CONST,
                                inst.location);
                return;
            }

            if (val_t &&
                !is_value_assignable(inst.operands[0], *val_t, *it->second.type, inst.location) &&
                !packed_backing_store(it->second.type, &*val_t)) {
                emit_diagnostic(format_store_mismatch(*val_t, *it->second.type),
                                error::TYPE_MISMATCH,
                                inst.location);
            }
        }
    } else if (inst.operands.size() >= 2) {
        // Case B: Storing through a pointer destination
        const auto dest_t{get_operand_type(inst.operands[0])};
        const auto val_t{get_operand_type(inst.operands[1])};
        if (dest_t) {
            if (dest_t->get_kind() == type_kind::POINTER) {
                const auto ptr_data{dest_t->get_data().as_opt<types::pointer>()};
                if (ptr_data) {
                    if (dest_t->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }

                    if (val_t &&
                        !is_value_assignable(
                            inst.operands[1], *val_t, ptr_data->underlying, inst.location)) {
                        emit_diagnostic(format_store_mismatch(*val_t, ptr_data->underlying),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                    }
                }
            } else if (dest_t->get_kind() == type_kind::REFERENCE) {
                const auto ref_data{dest_t->get_data().as_opt<types::reference>()};
                if (ref_data) {
                    if (inst.is_initializer) {
                        if (val_t && !is_value_assignable(
                                         inst.operands[1], *val_t, *dest_t, inst.location)) {
                            emit_diagnostic(format_store_mismatch(*val_t, *dest_t),
                                            error::TYPE_MISMATCH,
                                            inst.location);
                        }
                        return;
                    }

                    if (dest_t->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through reference",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }

                    if (val_t &&
                        !is_value_assignable(
                            inst.operands[1], *val_t, ref_data->underlying, inst.location)) {
                        emit_diagnostic(format_store_mismatch(*val_t, ref_data->underlying),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                    }
                }
            }
        }
    }
}

} // namespace ghoti::sema
