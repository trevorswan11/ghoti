#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <llvm/TargetParser/Triple.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/codegen/intrinsics.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/layout.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/peer_type.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/diagnostic.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

constexpr std::string_view DEFAULT_COMPILE_ERROR{"compilation aborted by @compileError"};
constexpr std::string_view DEFAULT_ASSERT_FAILURE{"Compile-time assertion failed"};

// The text of a string literal argument, none for any other argument
[[nodiscard]] auto string_literal(const module_t& m, gsl::span<const ref> args, usize index)
    -> stdx::option<std::string_view> {
    if (args.size() <= index) { return stdx::none; }
    const auto inst{as_inst(args[index])};
    if (!inst || m.tag_of(*inst) != tag::str) { return stdx::none; }
    return m.string(m.data_of(*inst).str);
}

} // namespace

namespace {

// Where a math or target builtin's value arguments start, none for a builtin that may take types
[[nodiscard]] auto value_args_from(syntax::builtin_t which) noexcept -> stdx::option<usize> {
    using syntax::builtin_t;
    switch (which) {
    case builtin_t::MUL_ADD: return 1UZ;
    case builtin_t::CLZ:
    case builtin_t::CTZ:
    case builtin_t::POPCOUNT:
    case builtin_t::SQRT:
    case builtin_t::SIN:
    case builtin_t::COS:
    case builtin_t::TAN:
    case builtin_t::EXP:
    case builtin_t::EXP2:
    case builtin_t::LOG:
    case builtin_t::LOG2:
    case builtin_t::LOG10:
    case builtin_t::FLOOR:
    case builtin_t::CEIL:
    case builtin_t::ABS:
    case builtin_t::MIN:
    case builtin_t::MAX:
    case builtin_t::DIV_TRUNC:
    case builtin_t::DIV_FLOOR:
    case builtin_t::DIV_EXACT:
    case builtin_t::REM:
    case builtin_t::MOD:
    case builtin_t::ADD_WITH_OVERFLOW:
    case builtin_t::SUB_WITH_OVERFLOW:
    case builtin_t::MUL_WITH_OVERFLOW:
    case builtin_t::SHL_WITH_OVERFLOW:
    case builtin_t::TARGET_TRIPLE:
    case builtin_t::TARGET_OS:
    case builtin_t::TARGET_ARCH:
    case builtin_t::TARGET_ABI:
    case builtin_t::TARGET_FAMILY:
    case builtin_t::OPTIMIZE_MODE:
    case builtin_t::RUNTIME_SAFETY:
    case builtin_t::BRANCH_HINT:
    case builtin_t::SET_RUNTIME_SAFETY:
    case builtin_t::TARGET_ENDIAN:
    case builtin_t::TARGET_PTR_BITS:
    case builtin_t::SET_EVAL_RECURSION_LIMIT:
    case builtin_t::SET_EVAL_UNROLL_LIMIT:
    case builtin_t::RETURN_ADDRESS: return 0UZ;
    default:                        return stdx::none;
    }
}

} // namespace

auto analyzer::reject_type_arguments(syntax::builtin_t    which,
                                     gsl::span<const ref> args,
                                     const source&        src) -> result<void> {
    // Each argument was analyzed before the call, so resolving it again only reads its result
    for (const auto arg : args) {
        const auto value{TRY(resolve(arg, src))};
        const auto known{comptime_value(value)};
        if (!known || !comp_.constants().is_type(*known)) { continue; }
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_USED_AS_VALUE,
                            fmt::format("'{}' expects a value, but was given the type '{}'",
                                        syntax::builtin_name(which),
                                        comp_.ctx().type_display_name(
                                            comp_.constants().type_at(*known))));
    }

    return {};
}

auto compile_error_message(const module_t& m, gsl::span<const ref> args) -> std::string {
    return std::string{string_literal(m, args, 0).value_or(DEFAULT_COMPILE_ERROR)};
}

auto analyzer::analyze_builtin(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto  which{static_cast<syntax::builtin_t>(rec.u32_field())};
    const auto  args{rec.refs_field()};
    const auto& src{src_of(inst)};
    using syntax::builtin_t;
    if (const auto first{value_args_from(which)}) {
        TRY(reject_type_arguments(which, args.subspan(std::min(*first, args.size())), src));
    }

    switch (which) {
    case builtin_t::COMPILE_ERROR:            return builtin_compile_error(args, src);
    case builtin_t::SET_EVAL_RECURSION_LIMIT: return builtin_limit(args, src, true);
    case builtin_t::SET_EVAL_UNROLL_LIMIT:    return builtin_limit(args, src, false);
    case builtin_t::SIZE_OF:                  return builtin_layout(args, src, true);
    case builtin_t::BIT_SIZE_OF:              return builtin_bit_size(args, src);
    case builtin_t::TYPE_NAME:                return builtin_type_name(args, src);
    case builtin_t::EMBED:                    return builtin_embed(args, src);
    case builtin_t::DYN_CAST:                 return builtin_dyn_cast(args, src);
    case builtin_t::TAG_NAME:                 return builtin_tag_name(args, src);
    case builtin_t::FIELD:                    return builtin_field(args, src);
    case builtin_t::HAS_FIELD:
    case builtin_t::FIELD_TYPE:
    case builtin_t::FIELD_DEFAULT:            return builtin_field_query(which, args, src);
    case builtin_t::FIELD_PARENT_PTR:         return builtin_field_parent_ptr(args, src);
    case builtin_t::TYPE_INFO:                return builtin_type_info(args, src);
    case builtin_t::ALIGN_OF:                 return builtin_layout(args, src, false);
    case builtin_t::LLVM_INTRINSIC:           return builtin_llvm_intrinsic(args, src);
    case builtin_t::RETURN_ADDRESS:           return builtin_return_address(src);
    case builtin_t::FN_CTX:                   return builtin_fn_ctx(src);
    case builtin_t::ENTRY_MAIN:               return builtin_entry_main(src);
    case builtin_t::ENTRY_TEST_RUNNER:        return builtin_entry_test_runner(src);
    case builtin_t::TEST_FUNCTIONS:           return builtin_test_functions(src);
    case builtin_t::EXPORT:                   return builtin_export(args, src);
    case builtin_t::SRC:                      return builtin_src(src);
    case builtin_t::SKIP:                     return builtin_skip(args, src);
    case builtin_t::EXPECT:                   return builtin_expect(args, src, false);
    case builtin_t::REQUIRE:                  return builtin_expect(args, src, true);
    case builtin_t::ASSERT:                   return builtin_assert(args, src, false);
    case builtin_t::VERIFY:                   return builtin_assert(args, src, true);
    case builtin_t::TRAP:                     return builtin_trap(src);
    case builtin_t::C_VA_START:
    case builtin_t::C_VA_COPY:
    case builtin_t::C_VA_END:
    case builtin_t::C_VA_ARG:                 return builtin_c_va(which, args, src);
    case builtin_t::PANIC:                    return builtin_panic(args, src);
    case builtin_t::IMPLEMENTS:               return builtin_implements(args, src);
    case builtin_t::AS:                       return builtin_as(inst, args, src);
    case builtin_t::THIS:                     return builtin_this(src);
    case builtin_t::INT:                      return builtin_int_type(args, src);
    case builtin_t::CLZ:
    case builtin_t::CTZ:
    case builtin_t::POPCOUNT:                 return builtin_bit_count(which, args, src);
    case builtin_t::ABS:                      return builtin_abs(args, src);
    case builtin_t::MUL_ADD:                  return builtin_mul_add(args, src);
    case builtin_t::ATOMIC_LOAD:
    case builtin_t::ATOMIC_STORE:
    case builtin_t::ATOMIC_RMW:
    case builtin_t::CMPXCHG_WEAK:
    case builtin_t::CMPXCHG_STRONG:
    case builtin_t::FENCE:                    return builtin_atomic(which, args, src);
    case builtin_t::BRANCH_HINT:              return builtin_branch_hint(inst, args, src);
    case builtin_t::SET_RUNTIME_SAFETY:       return builtin_set_runtime_safety(args, src);
    case builtin_t::RUNTIME_SAFETY:
        return operand{comp_.constants().intern(runtime_safety(), builtin(gs::type_kind::BOOL))};
    case builtin_t::ADD_WITH_OVERFLOW:
    case builtin_t::SUB_WITH_OVERFLOW:
    case builtin_t::MUL_WITH_OVERFLOW:
    case builtin_t::SHL_WITH_OVERFLOW: return builtin_overflow(which, args, src);
    case builtin_t::MEMCPY:
    case builtin_t::MEMMOVE:
    case builtin_t::MEMSET:            return builtin_mem(which, args, src);
    case builtin_t::MIN:
    case builtin_t::MAX:
    case builtin_t::DIV_TRUNC:
    case builtin_t::DIV_FLOOR:
    case builtin_t::DIV_EXACT:
    case builtin_t::REM:
    case builtin_t::MOD:               return builtin_binary_math(which, args, src);
    case builtin_t::SQRT:
    case builtin_t::SIN:
    case builtin_t::COS:
    case builtin_t::TAN:
    case builtin_t::EXP:
    case builtin_t::EXP2:
    case builtin_t::LOG:
    case builtin_t::LOG2:
    case builtin_t::LOG10:
    case builtin_t::FLOOR:
    case builtin_t::CEIL:              return builtin_float_math(which, args, src);
    case builtin_t::VECTOR:            return builtin_vector_type(args, src);
    case builtin_t::ENUM:              return builtin_enum(inst, args, src);
    case builtin_t::STRUCT:            return builtin_struct(inst, args, src);
    case builtin_t::UNION:             return builtin_union(inst, args, src);
    case builtin_t::TARGET_OS:
    case builtin_t::TARGET_ARCH:
    case builtin_t::TARGET_ABI:
    case builtin_t::TARGET_FAMILY:
    case builtin_t::TARGET_ENDIAN:
    case builtin_t::TARGET_TRIPLE:
    case builtin_t::TARGET_PTR_BITS:
    case builtin_t::OPTIMIZE_MODE:     return builtin_target(which, src);
    case builtin_t::FLOAT:             return builtin_float_type(args, src);
    case builtin_t::POINTER:
    case builtin_t::REFERENCE:
    case builtin_t::SLICE:
    case builtin_t::ARRAY:             return builtin_shaped_type(which, args, src);
    case builtin_t::FN:                return builtin_fn_type(args, src);
    case builtin_t::ENUM_LITERAL:      return interned_type(builtin(gs::type_kind::ENUM_LITERAL));
    case builtin_t::INT_CAST:
    case builtin_t::TRUNCATE:
    case builtin_t::BIT_CAST:
    case builtin_t::INT_FROM_BOOL:
    case builtin_t::BOOL_FROM_INT:
    case builtin_t::FROM_BACKING_INT:
    case builtin_t::BACKING_INT:
    case builtin_t::INT_FROM_FLOAT:
    case builtin_t::FLOAT_FROM_INT:
    case builtin_t::FLOAT_CAST:
    case builtin_t::PTR_CAST:
    case builtin_t::ALIGN_CAST:
    case builtin_t::PTR_FROM_INT:
    case builtin_t::INT_FROM_PTR:
    case builtin_t::CONST_CAST:
    case builtin_t::VOLATILE_CAST:
    case builtin_t::SLICE_FROM_PTR:
    case builtin_t::SPLAT:             return builtin_cast(inst, which, args);
    case builtin_t::REDUCE:            return builtin_reduce(args, src);
    case builtin_t::SELECT:            return builtin_select(args, src);
    case builtin_t::SHUFFLE:           return builtin_shuffle(args, src);
    case builtin_t::MASKED_LOAD:
    case builtin_t::MASKED_STORE:
    case builtin_t::GATHER:
    case builtin_t::SCATTER:           return builtin_masked(which, args, src);
    default:                           break;
    }

    return comp_.unsupported(u_, src, fmt::format("the `{}` builtin", syntax::builtin_name(which)));
}

auto analyzer::analyze_type_of(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  count{rec.count()};
    if (count == 0) {
        return comp_.report(u_,
                            src,
                            gs::error::ARITY_MISMATCH,
                            "'@TypeOf' expects at least 1 argument, found 0");
    }

    std::vector<gs::peer_operand> peers;
    std::vector<source>           sources;
    for (u32 i{0}; i < count; ++i) {
        const auto body{rec.body_field()};
        const auto value{TRY(unevaluated(body, src))};

        // A type operand's type is the type itself
        const auto known{comptime_value(value)};
        const auto generic{value.as_opt<generic_ref>()};
        const bool own_instance{generic && fn_ && fn_->owner.get() == generic->fn->owner.get() &&
                                fn_->func == generic->fn->func};
        // Another generic's type is its signature, its comptime and `auto` parts left open
        auto& type{own_instance ? *fn_->type
                   : generic    ? *TRY(comp_.generic_value_type(*generic->fn))
                   : known && comp_.constants().is_type(*known) ? comp_.constants().type_at(*known)
                                                                : type_of(value)};
        if (type.is_poison()) { return stdx::err{stop::POISON}; }

        peers.emplace_back<gs::peer_operand>({.type = &type});
        sources.emplace_back(body.empty() ? src : src_of(body.back()));
    }

    auto&      ctx{comp_.ctx()};
    const auto peer{gs::peer_type(ctx, peers)};
    if (!peer) {
        return comp_.report(
            u_,
            sources[peer.error().second],
            gs::error::NO_PEER_TYPE,
            fmt::format("'@TypeOf': {}", gs::peer_error_message(ctx, peers, peer.error())));
    }

    return interned_type(**peer);
}

auto analyzer::unevaluated(gsl::span<const inst_id> body, const source& src) -> result<operand> {
    // A runtime body's operand is analyzed into a function no one emits
    if (!gir_fn_) {
        const auto outer{std::exchange(only_types_, true)};
        const auto restore{gsl::finally([&] { only_types_ = outer; })};
        const auto out{TRY(analyze_body(body))};
        if (!out.yielded) { return comp_.unsupported(u_, src, "an operand that doesn't yield"); }
        return *out.yielded;
    }

    const auto out{TRY(analyze_unemitted(body))};
    if (!out.yielded) { return comp_.unsupported(u_, src, "an operand that doesn't yield"); }
    return *out.yielded;
}

auto analyzer::analyze_unemitted(gsl::span<const inst_id> body) -> result<flow> {
    gir::function scratch{
        comp_.ctx().arena, comp_.constants(), std::string{}, builtin(gs::type_kind::VOID_)};
    const auto outer_fn{gir_fn_};
    const auto outer_segment{builder_.get_segment()};
    const auto restore{gsl::finally([&] {
        gir_fn_ = outer_fn;
        if (outer_fn && outer_segment) { builder_.set_insert_point(*outer_fn, *outer_segment); }
    })};
    const auto depth{std::exchange(comptime_depth_, 0)};
    const auto restore_depth{gsl::finally([&] { comptime_depth_ = depth; })};
    gir_fn_.emplace(scratch);
    builder_.set_insert_point(scratch, scratch.add_segment());
    return analyze_body(body);
}

auto analyzer::hint_inst_of(gsl::span<const inst_id> body) const -> stdx::option<inst_id> {
    for (const auto inst : body) {
        const auto t{mod().tag_of(inst)};
        if (t == tag::implicit_access || t == tag::str) { continue; }
        if (t == tag::block) {
            auto rec{record(inst)};
            rec.skip_str();
            return hint_inst_of(rec.body_field());
        }

        if (t != tag::builtin) { return stdx::none; }
        auto       rec{record(inst)};
        const auto which{static_cast<syntax::builtin_t>(rec.u32_field())};
        return which == syntax::builtin_t::BRANCH_HINT ? stdx::option<inst_id>{inst} : stdx::none;
    }

    return stdx::none;
}

auto analyzer::branch_hint_of(gsl::span<const inst_id> body) const
    -> stdx::option<ast::branch_hint> {
    const auto inst{hint_inst_of(body)};
    if (!inst) { return stdx::none; }
    auto rec{record(*inst)};
    rec.skip_u32();
    const auto args{rec.refs_field()};
    const auto arg{args.empty() ? stdx::none : as_inst(args.front())};
    if (!arg || mod().tag_of(*arg) != tag::implicit_access) { return stdx::none; }
    return ast::branch_hint_from_name(mod().string(mod().data_of(*arg).str));
}

auto analyzer::apply_branch_hints(gir::instruction&        branch,
                                  gsl::span<const inst_id> on_true,
                                  gsl::span<const inst_id> on_false) const -> void {
    static constexpr u32 LIKELY_WEIGHT{2'000};
    static constexpr u32 UNLIKELY_WEIGHT{1};
    const auto           weight_of{[](stdx::option<ast::branch_hint> hint) -> stdx::option<u32> {
        if (hint == ast::branch_hint::LIKELY) { return LIKELY_WEIGHT; }
        if (hint == ast::branch_hint::UNLIKELY || hint == ast::branch_hint::COLD) {
            return UNLIKELY_WEIGHT;
        }
        return stdx::none;
    }};
    const auto           opposite{
        [](u32 weight) { return weight == LIKELY_WEIGHT ? UNLIKELY_WEIGHT : LIKELY_WEIGHT; }};

    const auto true_hint{branch_hint_of(on_true)};
    const auto false_hint{branch_hint_of(on_false)};
    const auto true_weight{weight_of(true_hint)};
    const auto false_weight{weight_of(false_hint)};
    if (true_weight || false_weight) {
        branch.branch_weights.emplace(true_weight.value_or(opposite(*false_weight)),
                                      false_weight.value_or(opposite(*true_weight)));
    }
    branch.unpredictable = true_hint == ast::branch_hint::UNPREDICTABLE ||
                           false_hint == ast::branch_hint::UNPREDICTABLE;
}

auto analyzer::builtin_branch_hint(inst_id inst, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@branchHint` of this arity"); }
    const auto arg_at{arg_src(src, 0).value_or(src)};
    auto&      hint_type{*TRY(comp_.prelude_type("BranchHint", u_, src))};
    const auto given{TRY(resolve_value(args[0], src))};
    const auto value{[&]() -> result<stdx::option<ip::index>> {
        const auto scope{comptime_scope()};
        return comptime_value(
            TRY(coerce_to(given, hint_type, arg_at, {.what = use_site::kind::STORE})));
    }()};
    if (!value && value.error() != stop::RUNTIME) { return stdx::err{value.error()}; }
    const auto variant{value && *value ? variant_name(**value) : stdx::none};
    if (!variant) {
        return comp_.report(u_,
                            arg_at,
                            gs::error::TYPE_MISMATCH,
                            "@branchHint requires a compile-time 'builtin.BranchHint'");
    }

    // A hint leads an `if` / `else` branch, a `match` arm or a function body
    if (body_hints_.empty() || body_hints_.back() != inst) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            "@branchHint must be the first statement of an 'if'/'else' branch, a "
                            "'match' arm, or a function body");
    }

    const auto hint{ast::branch_hint_from_name(*variant)};
    if (body_hints_.size() == 1 && hint != ast::branch_hint::COLD &&
        hint != ast::branch_hint::NONE) {
        return comp_.report(u_,
                            arg_at,
                            gs::error::TYPE_MISMATCH,
                            "A function body only accepts '@branchHint(.cold)'");
    }

    return void_value();
}

auto analyzer::runtime_safety() const -> bool {
    for (const auto& scope : std::views::reverse(safety_scopes_)) {
        if (scope) { return *scope; }
    }

    return comp_.ctx().runtime_safety;
}

auto analyzer::builtin_set_runtime_safety(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (args.size() != 1) {
        return comp_.unsupported(u_, src, "a `@setRuntimeSafety` of this arity");
    }

    if (!gir_fn_ || !fn_ || safety_scopes_.empty()) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            "@setRuntimeSafety can only be used within a function body");
    }

    const auto arg_at{arg_src(src, 0).value_or(src)};
    const auto given{TRY(resolve_value(args[0], src))};
    const auto known{comptime_value(given)};
    const auto enabled{known ? comp_.constants().value_at(*known).data.as_opt<bool>() : stdx::none};
    if (!enabled) {
        return comp_.report(u_,
                            arg_at,
                            gs::error::TYPE_MISMATCH,
                            "@setRuntimeSafety requires a compile-time 'bool'");
    }

    // The body holding the call is checked as it says from here on, with what it contains
    safety_scopes_.back() = *enabled;
    return void_value();
}

auto analyzer::builtin_compile_error(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (!args.empty() && !string_literal(mod(), args, 0)) {
        return comp_.unsupported(u_, src, "a `@compileError` message that isn't a literal");
    }

    return comp_.report(
        u_, src, gs::error::COMPILE_ERROR_REACHED, compile_error_message(mod(), args));
}

auto analyzer::builtin_limit(gsl::span<const ref> args, const source& src, bool recursion)
    -> result<operand> {
    // A limit that isn't a known positive integer leaves the current one
    if (!args.empty()) {
        const auto value{resolve_value(args.front(), src)};
        const auto index{value ? comptime_value(*value) : stdx::none};
        const auto limit{index ? int_of(comp_.constants().value_at(*index)) : stdx::none};
        const auto small{limit ? limit->to_i128() : stdx::none};
        if (small && *small > 0) {
            auto& limits{comp_.limits()};
            (recursion ? limits.recursion : limits.unroll) = static_cast<usize>(*small);
        }
    }

    return void_value();
}

auto analyzer::builtin_layout(gsl::span<const ref> args, const source& src, bool size)
    -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a layout builtin's arity"); }
    auto& type{*TRY(resolve_type(args.front(), src))};
    TRY(layout_ready(type, src));
    const auto  layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
    const usize bytes{size ? gir::size_of(type, layout) : gir::align_of(type, layout)};
    return intern_int(
        comp_.constants(), big_int{static_cast<u128>(bytes)}, builtin(gs::type_kind::COMPTIME_INT));
}

auto analyzer::builtin_bit_size(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a layout builtin's arity"); }
    auto& type{*TRY(resolve_type(args.front(), src))};
    TRY(layout_ready(type, src));
    const auto layout{codegen::type_layout::resolve(comp_.ctx().target_opts.triple_str)};
    const auto ptr_bits{static_cast<u32>(layout.ptr_bytes) * 8};
    const auto kind{type.get_kind()};
    const auto bits{[&] -> u64 {
        if (kind == gs::type_kind::INT) { return gs::int_width(type); }
        if (kind == gs::type_kind::ISIZE || kind == gs::type_kind::USIZE) { return ptr_bits; }
        if (kind == gs::type_kind::BOOL) { return 1; }
        if (gs::is_float(kind)) { return gs::float_bits(kind); }
        if (const auto st{type.get_data().as_opt<gs::types::struct_t>()};
            st && st->is_bit_packed()) {
            if (const auto n{gs::packed_backing_bits(*st, ptr_bits)}) { return *n; }
        }

        if (const auto ut{type.get_data().as_opt<gs::types::union_t>()};
            ut && ut->is_bit_packed()) {
            if (const auto n{gs::packed_union_backing_bits(*ut, ptr_bits)}) { return *n; }
        }

        return static_cast<u64>(gir::size_of(type, layout)) * 8;
    }()};
    return intern_int(
        comp_.constants(), big_int{static_cast<u128>(bits)}, builtin(gs::type_kind::COMPTIME_INT));
}

auto analyzer::builtin_type_name(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@typeName` of this arity"); }
    auto&       type{*TRY(resolve_type(args.front(), src))};
    auto&       ctx{comp_.ctx()};
    std::string name{ctx.type_display_name(type)};
    auto& array{ctx.get_array(gs::types::mut::CONSTANT, true, name.size(), ctx.get_int(8, false))};
    return comp_.constants().intern(std::move(name), array);
}

auto analyzer::builtin_embed(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "an `@embed` of this arity"); }
    const auto arg_at{arg_src(src, 0).value_or(src)};
    const auto path{[&]() -> result<stdx::option<std::string>> {
        const auto scope{comptime_scope()};
        const auto known{comptime_value(TRY(resolve_value(args.front(), arg_at)))};
        return known ? known_text(*known) : stdx::none;
    }()};
    if (!path && path.error() != stop::RUNTIME) { return stdx::err{path.error()}; }
    if (!path || !*path) {
        return comp_.report(u_,
                            arg_at,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "@embed argument must be a compile-time string");
    }

    auto&      ctx{comp_.ctx()};
    const auto full{u_.source->make_path_absolute(**path)};
    const auto content{ctx.read_embed_file(full)};
    if (!content) {
        return comp_.report(u_,
                            arg_at,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            fmt::format("failed to read embedded file '{}'", full.string()));
    }
    auto& array{
        ctx.get_array(gs::types::mut::CONSTANT, true, content->size(), ctx.get_int(8, false))};
    return comp_.constants().intern(std::string{*content}, array);
}

auto analyzer::builtin_tag_name(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a `@tagName` of this arity"); }
    const auto value{TRY(resolve_value(args.front(), src))};
    auto&      ctx{comp_.ctx()};
    auto&      name_type{ctx.get_slice(gs::types::mut::CONSTANT, true, ctx.get_int(8, false))};
    if (const auto literal{value.as_opt<enum_literal>()}) {
        return comp_.constants().intern(literal_name(*literal), name_type);
    }

    auto&      type{type_of(value)};
    const auto tagged{type.get_data().as_opt<gs::types::union_t>()};
    if (type.get_kind() != gs::type_kind::ENUM && (!tagged || tagged->is_untagged)) {
        if (type.is_poison()) { return stdx::err{stop::POISON}; }
        return comp_.report(
            u_,
            src,
            gs::error::TYPE_MISMATCH,
            fmt::format("'@tagName' expects an enum or tagged union value; found '{}'",
                        ctx.type_display_name(type)));
    }

    const auto index{comptime_value(value)};
    if (!index) { return runtime_tag_name(value, type, name_type, src); }
    const auto& data{comp_.constants().value_at(*index).data};
    if (const auto un{data.as_opt<ip::union_value>()}) {
        return comp_.constants().intern(std::string{un->active_field}, name_type);
    }

    const auto variant{variant_name(*index)};
    if (!variant) { return comp_.unsupported(u_, src, "a `@tagName` of this value"); }
    return comp_.constants().intern(std::string{*variant}, name_type);
}

auto analyzer::runtime_tag_name(const operand& value,
                                gs::type&      type,
                                gs::type&      name_type,
                                const source&  src) -> result<operand> {
    TRY(require_runtime(src));
    TRY(comp_.require_fields(u_, src, type));
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto merge{new_segment()};
    const auto slot{builder_.emit_alloca(name_type)};

    // Each name is tested in turn, storing the one that matches
    const auto name_case{[&](gir::value matches, std::string_view name) {
        const auto body{new_segment()};
        const auto next{new_segment()};
        builder_.emit_cond_goto(matches, body, next);
        switch_to(body);
        builder_.emit_store(gir::value{slot, name_type}, string_slice(name, name_type));
        emit_goto(merge);
        switch_to(next);
    }};
    const auto equal{[&](const gir::value& lhs, ip::index rhs, gs::type& as) {
        return gir::value{
            builder_.emit_binary(gir::instruction_kind::EQ, lhs, constant(rhs, as), bool_type),
            bool_type};
    }};

    auto& pool{comp_.constants()};
    if (const auto en{type.get_data().as_opt<gs::types::enum_t>()}) {
        auto&            underlying{en->underlying};
        const gir::value runtime{TRY(as_runtime(value, src))};
        const gir::value raw{
            builder_.emit_cast(gir::instruction_kind::BIT_CAST, runtime, underlying), underlying};
        const auto& values{comp_.nominal_of(type)->enum_values};
        for (usize i{0}; i < en->variant_names.size(); ++i) {
            name_case(equal(raw, pool.intern(values[i], underlying), underlying),
                      en->variant_names[i]);
        }

        // A non-exhaustive enum may hold a value no variant names
        if (en->non_exhaustive) {
            builder_.emit_store(gir::value{slot, name_type}, string_slice("_", name_type));
            emit_goto(merge);
        } else {
            builder_.emit_unreachable();
        }
    } else {
        const auto& ut{type.get_data().as<gs::types::union_t>()};
        const auto  base{TRY(operand_address(value, src)).address};
        auto&       i32_type{comp_.ctx().get_int(32, true)};
        const auto  tag_at{
            builder_.emit_get_element_ptr(base, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
        const gir::value tag{builder_.emit_load(gir::value{tag_at, i32_type}, i32_type), i32_type};
        for (usize i{0}; i < ut.field_names.size(); ++i) {
            name_case(equal(tag, pool.intern(static_cast<i64>(i), i32_type), i32_type),
                      ut.field_names[i]);
        }

        builder_.emit_unreachable();
    }

    switch_to(merge);
    return gir::value{builder_.emit_load(gir::value{slot, name_type}, name_type), name_type};
}

auto analyzer::layout_ready(gs::type& type, const source& src) -> result<void> {
    // A type whose fields are still resolving can't be sized, and asking is a cycle
    if (const auto n{comp_.nominal_of(type)}; n && n->state == nominal_state::FIELDS_RESOLVING) {
        return comp_.report(u_,
                            src,
                            gs::error::CYCLIC_DEPENDENCY,
                            fmt::format("the layout of '{}' depends on itself", type.to_string()));
    }

    TRY(comp_.require_fields(u_, src, type));

    // Every by-value part of an aggregate must be sized too
    if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        for (const auto field : st->fields) { TRY(layout_ready(*field, src)); }
    } else if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
        for (const auto field : ut->fields) { TRY(layout_ready(*field, src)); }
    } else if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        TRY(layout_ready(arr->underlying, src));
    }

    return {};
}

auto analyzer::builtin_as(inst_id inst, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    // `@as(x)` converts to the type it is used as
    if (args.size() == 1) { return cast_ref{inst}; }
    if (args.size() != 2) { return comp_.unsupported(u_, src, "an `@as` of this arity"); }
    return as_to(*TRY(resolve_type(args[0], src)), args[1], src);
}

auto analyzer::as_to(gs::type& target, ref value_ref, const source& src) -> result<operand> {
    auto value{TRY(resolve_value(value_ref, src))};
    TRY(reject_runtime_to_comptime(value, target, src));

    // A vector takes its lanes from a list, an array or another vector
    if (gs::is_vector(target)) {
        return coerce_to(value, target, src, {.what = use_site::kind::STORE});
    }

    // An inferred cast or literal takes `@as`'s type first, then is cast like a typed value
    if (is_untyped_literal(value)) {
        value = TRY(coerce_to(value, target, src, {.what = use_site::kind::STORE}));
    }

    auto& from{type_of(value)};
    if (from.is_poison() || target.is_poison()) { return stdx::err{stop::POISON}; }
    if (!gs::is_numeric(from.get_kind()) || !gs::is_numeric(target.get_kind())) {
        return comp_.unsupported(u_, src, "an `@as` between non-numeric types");
    }

    // A negative untyped integer that the same-width signed type holds wraps into unsigned
    if (const auto index{comptime_value(value)};
        index && from.get_kind() == gs::type_kind::COMPTIME_INT &&
        gs::is_unsigned_integer(target) && domain_of(target).bits < 127) {
        const auto exact{int_of(comp_.constants().value_at(*index))};
        const auto small{exact ? exact->to_i128() : stdx::none};
        const i128 span{i128{1} << domain_of(target).bits};
        if (small && *small < 0 && *small >= -(span / 2)) {
            return operand{intern_int(comp_.constants(), big_int{*small + span}, target)};
        }
    }

    // A known typed number widens exactly into an untyped one
    if (const auto index{comptime_value(value)}; index &&
                                                 gs::is_comptime_numeric(target.get_kind()) &&
                                                 !gs::is_comptime_numeric(from.get_kind())) {
        auto&      pool{comp_.constants()};
        const auto data{pool.value_at(*index).data};
        if (target.get_kind() == gs::type_kind::COMPTIME_FLOAT && gs::is_float(from.get_kind())) {
            if (const auto f{data.as_opt<f128>()}) { return operand{pool.intern(*f, target)}; }
        }

        if (target.get_kind() == gs::type_kind::COMPTIME_INT && gs::is_integer(from.get_kind())) {
            if (const auto exact{int_of(pool.value_at(*index))}) {
                return operand{intern_int(pool, *exact, target)};
            }
        }
    }

    // A known operand converts as a value, which also checks it fits
    if (gs::is_comptime_numeric(from.get_kind()) || is_comptime()) {
        return coerce_to(value, target, src, {.what = use_site::kind::STORE});
    }

    // A known value may narrow when it fits, and is still cast where it is used
    if (!gs::is_same_unqualified(from, target) && !gs::is_implicit_widenable(from, target)) {
        if (!comptime_value(value)) {
            return comp_.report(u_, src, gs::error::TYPE_MISMATCH, narrowing_as(from, target));
        }

        // A runtime one narrows through the builtin that says how
        DISCARD(TRY(coerce_to(value, target, src, {.what = use_site::kind::STORE})));
    }

    // Emit the cast even the value is already of the target type
    const auto operand{TRY(as_runtime(value, src))};
    return gir::value{builder_.emit_cast(gir::instruction_kind::WIDEN_CAST, operand, target),
                      target};
}

auto analyzer::narrowing_as(const gs::type& from, const gs::type& target) -> std::string {
    auto&      ctx{comp_.ctx()};
    const auto from_name{ctx.type_display_name(from)};
    const auto target_name{ctx.type_display_name(target)};
    const bool to_float{gs::is_float(target.get_kind())};
    const bool from_float{gs::is_float(from.get_kind())};
    if (to_float && !from_float) {
        return fmt::format("`@as` cannot convert '{}' to '{}' exactly; use `@floatFromInt` instead",
                           from_name,
                           target_name);
    }

    if (from_float && !to_float) {
        return "`@as` cannot convert a float to an integer; use `@intFromFloat` instead";
    }

    return fmt::format("`@as` cannot narrow '{}' to '{}'; use `{}`",
                       from_name,
                       target_name,
                       to_float ? "@floatCast" : "@intCast");
}

auto analyzer::builtin_this(const source& src) -> result<operand> {
    // The type whose namespace holds the declaration being analyzed
    if (self_type_) { return interned_type(*self_type_); }
    if (scope_) { return interned_type(*scope_->type); }
    const auto decl{fn_ ? stdx::option<decl_id>{fn_->decl} : declaring_};
    const auto owner{decl ? mod().decl_at(*decl).owner : stdx::none};
    if (!owner) { return comp_.unsupported(u_, src, "`@This` outside a type"); }
    const auto kind{[&] {
        switch (mod().tag_of(*owner)) {
        case tag::union_decl: return gs::type_kind::UNION;
        case tag::enum_decl:  return gs::type_kind::ENUM;
        default:              return gs::type_kind::STRUCT;
        }
    }()};
    if (mod().tag_of(*owner) == tag::interface_decl || mod().tag_of(*owner) == tag::impl_decl) {
        return comp_.unsupported(u_, src, "`@This` in an interface or impl");
    }

    // A member runs inside a comptime call of its own, but its type was declared outside it
    if (const auto existing{comp_.existing_nominal(u_, *owner)}) {
        return interned_type(*existing->type);
    }

    return interned_type(*comp_.declare_nominal(u_, *owner, kind).type);
}

auto analyzer::builtin_c_va(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
    -> result<operand> {
    const auto name{syntax::builtin_name(which)};
    auto&      ctx{comp_.ctx()};
    const auto lists{which == syntax::builtin_t::C_VA_COPY ? 2UZ : 1UZ};
    const auto wanted{which == syntax::builtin_t::C_VA_ARG ? 2UZ : lists};
    if (args.size() != wanted) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Builtin expects {} arguments, found {}", wanted, args.size()));
    }

    // Every va builtin addresses caller-provided `va_list` storage through a pointer
    std::vector<gir::value> values;
    for (usize i{0}; i < lists; ++i) {
        const auto list{TRY(resolve_value(args[i], src))};
        auto&      list_type{type_of(list)};
        if (list_type.get_kind() != gs::type_kind::POINTER || list_type.is_constant()) {
            return comp_.report(
                u_,
                src,
                gs::error::TYPE_MISMATCH,
                fmt::format("'{}' expects a `^mut` pointer to `va_list` storage; found '{}'",
                            name,
                            ctx.type_display_name(list_type)));
        }
        values.emplace_back(TRY(as_runtime(list, src)));
    }

    auto& result_type{which == syntax::builtin_t::C_VA_ARG ? *TRY(resolve_type(args[1], src))
                                                           : builtin(gs::type_kind::VOID_)};
    if (which == syntax::builtin_t::C_VA_START &&
        !fn_->type->get_data().as<gs::types::function>().is_variadic) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            "'@cVaStart' can only be used inside a C-variadic `fn(..., ...)`");
    }

    TRY(require_runtime(src));
    const auto result{builder_.emit_builtin_call(name, std::move(values), result_type)};
    if (!result) { return void_value(); }
    return gir::value{*result, result_type};
}

auto analyzer::builtin_trap(const source& src) -> result<operand> {
    TRY(require_runtime(src));
    auto& noreturn_type{builtin(gs::type_kind::NORETURN)};
    DISCARD(builder_.emit_builtin_call("@trap", {}, noreturn_type));
    builder_.emit_unreachable();
    return void_value();
}

auto analyzer::builtin_assert(gsl::span<const ref> args, const source& src, bool verify)
    -> result<operand> {
    if (args.empty()) { return comp_.unsupported(u_, src, "an `@assert` without a condition"); }
    if (!is_comptime()) { return runtime_assert(args, src, verify); }
    const auto cond{TRY(resolve_value(args.front(), src))};
    if (TRY(known_bool(cond, src))) { return void_value(); }

    // Outside a call the old resolver folds the condition itself and reports it as static
    if (!comp_.in_comptime_call()) {
        const auto text{string_literal(mod(), args, 1)};
        return comp_.report(u_,
                            src,
                            gs::error::STATIC_ASSERTION_FAILED,
                            fmt::format("{} failed at compile time{}",
                                        verify ? "@verify" : "@assert",
                                        text ? fmt::format(": {}", *text) : std::string{}));
    }

    return comp_.report(
        u_,
        src,
        gs::error::COMPTIME_EVALUATION_FAILED,
        std::string{string_literal(mod(), args, 1).value_or(DEFAULT_ASSERT_FAILURE)});
}

auto analyzer::builtin_llvm_intrinsic(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (args.size() < 2) {
        return comp_.report(
            u_,
            src,
            gs::error::ARITY_MISMATCH,
            fmt::format("Builtin expects at least 2 arguments, found {}", args.size()));
    }
    TRY(require_runtime(src));

    // The name is a compile-time string
    const auto name{TRY([&] -> result<stdx::option<std::string>> {
        if (const auto literal{string_literal(mod(), args, 0)}) {
            return stdx::option<std::string>{std::string{*literal}};
        }
        const auto value{[&] {
            const auto scope{comptime_scope()};
            return resolve_value(args[0], src);
        }()};
        const auto known{value ? comptime_value(*value) : stdx::none};
        if (!known || comp_.constants().is_type(*known)) { return stdx::option<std::string>{}; }
        return comp_.constants().value_at(*known).data.as_opt<std::string>().transform(
            [](const std::string& s) { return s; });
    }())};
    if (!name) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "'@llvmIntrinsic' needs a compile-time string naming the intrinsic, "
                            "like \"llvm.x86.sse2.pause\"");
    }

    // The call carries the intrinsic's name, with the result type written second
    auto&                                      result_type{*TRY(resolve_type(args[1], src))};
    std::vector<gir::value>                    values;
    std::vector<gsl::not_null<const gs::type*>> types;
    std::vector<bool>                          known;
    for (const auto arg : args.subspan(2)) {
        const auto given{TRY(resolve_value(arg, src))};
        if (const auto index{comptime_value(given)}; index && comp_.constants().is_type(*index)) {
            return comp_.report(u_,
                                src,
                                gs::error::TYPE_USED_AS_VALUE,
                                "'@llvmIntrinsic' arguments are values, not types");
        }

        // An untyped literal is an `i32` or `f64`, as when it binds an `auto` parameter
        known.emplace_back(comptime_value(given).has_value());
        const auto typed{TRY(concrete(given, src))};
        const auto value{TRY(as_runtime(typed, src))};
        types.emplace_back(value.type.get());
        values.emplace_back(value);
    }

    std::vector<usize> immediates;
    if (const auto problem{codegen::check_llvm_intrinsic(
            *name,
            result_type,
            types,
            codegen::resolve_target_triple(comp_.ctx().target_opts.triple_str),
            immediates)}) {
        return comp_.report(u_, src, gs::error::INVALID_LLVM_INTRINSIC, *problem);
    }

    // LLVM takes these as part of the instruction, so they must be known now
    for (const auto index : immediates) {
        if (index < known.size() && !known[index]) {
            return comp_.report(
                u_,
                src,
                gs::error::COMPTIME_EVALUATION_FAILED,
                fmt::format("argument {} of '{}' must be known at compile time", index + 1, *name));
        }
    }

    const auto result{
        builder_.emit_builtin_call(std::string{*name}, std::move(values), result_type)};
    if (!result) { return void_value(); }
    return gir::value{*result, result_type};
}

auto analyzer::builtin_field(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a `@field` of this arity"); }

    // The name is a compile-time string, read like `object.name`
    auto name_value{TRY(resolve_value(args[1], src))};
    if (!comptime_value(name_value) && !is_comptime()) {
        // A runtime name, such as a call's result, may still fold
        auto folded{[&] {
            const auto scope{comptime_scope()};
            return reevaluate(args[1], src);
        }()};
        if (folded) { name_value = std::move(*folded); }
    }

    const auto known{comptime_value(name_value)};
    const auto name{known ? comp_.constants().value_at(*known).data.as_opt<std::string>()
                          : stdx::none};
    if (!name) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "'@field' needs a field name known at compile time");
    }

    // `@field` reads a field through a pointer without checking it for null
    const auto field_name{comp_.lasting_name(*name)};
    return field_value(args[0], field_name, src, false);
}

auto analyzer::field_name_arg(ref arg, const source& src) -> result<stdx::option<std::string>> {
    const auto given{[&]() -> result<operand> {
        const auto scope{comptime_scope()};
        return resolve_value(arg, src);
    }()};
    if (!given && given.error() != stop::RUNTIME) { return stdx::err{given.error()}; }
    const auto known{given ? comptime_value(*given) : stdx::none};
    if (!known || comp_.constants().is_type(*known)) { return stdx::option<std::string>{}; }
    const auto text{comp_.constants().value_at(*known).data.as_opt<std::string>()};
    return text ? stdx::option<std::string>{*text} : stdx::none;
}

auto analyzer::builtin_field_query(syntax::builtin_t    which,
                                   gsl::span<const ref> args,
                                   const source&        src) -> result<operand> {
    using syntax::builtin_t;
    const auto prefix{syntax::builtin_name(which)};
    if (args.size() != 2) { return comp_.unsupported(u_, src, "a field builtin's arity"); }
    const auto name_at{arg_src(src, 1).value_or(src)};
    auto&      pool{comp_.constants()};

    // `@hasField` asks of any type, reading no value
    if (which == builtin_t::HAS_FIELD) {
        const auto owner{TRY(resolve_value(args[0], src))};
        const auto owner_index{comptime_value(owner)};
        if (!owner_index || !pool.is_type(*owner_index)) {
            return comp_.report(u_,
                                arg_src(src, 0).value_or(src),
                                gs::error::TYPE_MISMATCH,
                                "@hasField: the first argument must be a type");
        }

        const auto name{TRY(field_name_arg(args[1], src))};
        if (!name) {
            return comp_.report(u_,
                                name_at,
                                gs::error::TYPE_MISMATCH,
                                "@hasField: the field name must be a compile-time-known string");
        }

        auto& type{pool.type_at(*owner_index)};
        if (comp_.nominal_of(type)) { TRY(comp_.require_fields(u_, src, type)); }
        return operand{pool.intern(gs::find_aggregate_field(type, *name).has_value(),
                                   builtin(gs::type_kind::BOOL))};
    }

    auto&      type{*TRY(resolve_type(args[0], src))};
    const auto name{TRY(field_name_arg(args[1], src))};
    if (comp_.nominal_of(type)) { TRY(comp_.require_fields(u_, src, type)); }
    const auto field{name ? gs::find_aggregate_field(type, *name) : stdx::none};
    if (!field) {
        return comp_.report(u_,
                            name_at,
                            gs::error::FIELD_NOT_FOUND,
                            fmt::format("{}: unknown field (the name must be a compile-time-known "
                                        "string)",
                                        prefix));
    }

    if (which == builtin_t::FIELD_TYPE) { return interned_type(field->field_type); }

    // `@fieldDefault` is the field's own default, of the field's type
    const auto defaults{type.get_data().is<gs::types::struct_t>() ? TRY(field_defaults(type, src))
                                                                  : std::vector<default_source>{}};
    if (field->index >= defaults.size() || !defaults[field->index].present()) {
        return comp_.report(u_,
                            name_at,
                            gs::error::FIELD_HAS_NO_DEFAULT,
                            "@fieldDefault: field has no default value");
    }

    return field_default(type, field->index, defaults[field->index], src);
}

auto analyzer::builtin_field_parent_ptr(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    if (args.size() != 3) {
        return comp_.unsupported(u_, src, "a `@fieldParentPtr` of this arity");
    }
    auto&      ctx{comp_.ctx()};
    auto&      parent{*TRY(resolve_type(args[0], src))};
    const auto display{[&](const gs::type& t) { return ctx.type_display_name(t); }};
    if (!parent.get_data().is<gs::types::struct_t>()) {
        return comp_.report(
            u_,
            arg_src(src, 0).value_or(src),
            gs::error::TYPE_MISMATCH,
            fmt::format("'@fieldParentPtr' expects a struct type; found '{}'", display(parent)));
    }

    const auto name_at{arg_src(src, 1).value_or(src)};
    const auto name{string_literal(mod(), args, 1)};
    if (!name) {
        return comp_.report(u_,
                            name_at,
                            gs::error::TYPE_MISMATCH,
                            "'@fieldParentPtr' expects a string-literal field name");
    }

    if (comp_.nominal_of(parent)) { TRY(comp_.require_fields(u_, src, parent)); }
    const auto field{gs::find_aggregate_field(parent, *name)};
    if (!field) {
        return comp_.report(u_,
                            name_at,
                            gs::error::UNKNOWN_FIELD,
                            fmt::format("'{}' is not a field of the given struct", *name));
    }

    const auto ptr{TRY(resolve_value(args[2], src))};
    auto&      ptr_type{type_of(ptr)};
    if (ptr_type.get_kind() != gs::type_kind::POINTER) {
        return comp_.report(u_,
                            arg_src(src, 2).value_or(src),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'@fieldParentPtr' expects a field pointer; found '{}'",
                                        display(ptr_type)));
    }

    // The parent's address is the field's minus its offset
    TRY(require_runtime(src));
    auto&                   result_type{ctx.get_pointer(
        ptr_type.is_constant() ? gs::types::mut::CONSTANT : gs::types::mut::MUTABLE, parent)};
    std::vector<gir::value> call_args;
    call_args.emplace_back(TRY(as_runtime(ptr, src)));
    call_args.emplace_back(usize_constant(field->index));
    const auto result{
        builder_.emit_builtin_call("@fieldParentPtr", std::move(call_args), result_type)};
    return operand{gir::value{*result, result_type}};
}

auto analyzer::builtin_return_address(const source& src) -> result<operand> {
    TRY(require_runtime(src));
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    const auto result{builder_.emit_builtin_call("@returnAddress", {}, usize_type)};
    return gir::value{*result, usize_type};
}

auto analyzer::builtin_name_location(const source& src) -> source_location {
    // A check is located at the builtin's name, not its call
    const auto callee{src.as_opt<ast::node_id>().and_then([&](ast::node_id id) {
        return u_.source->ast.get_as_opt<ast::call_expr>(id).transform(
            [](const ast::call_expr& call) { return source{call.function}; });
    })};
    return comp_.location_of(u_, callee.value_or(src)).one_based();
}

auto analyzer::builtin_expect(gsl::span<const ref> args, const source& src, bool require)
    -> result<operand> {
    if (args.empty() || args.size() > 2) {
        return comp_.unsupported(u_, src, "an `@expect` of this arity");
    }

    if (is_comptime()) { return comptime_expect(args, src, require); }

    auto&      ctx{comp_.ctx()};
    const auto cond{TRY(resolve_value(args.front(), src))};
    const auto checked{TRY(condition(cond, src))};
    auto&      pool{comp_.constants()};
    auto&      u32_type{ctx.get_int(32, false)};
    auto&      text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    gir::value message{constant(pool.intern(std::string{}, text_type), text_type)};
    if (args.size() > 1) {
        const auto given{TRY(resolve_value(args[1], src))};
        message = TRY(as_runtime(given, src));
    }

    // The failure branch calls a weak handler a test runner may replace
    DISCARD(TRY(comp_.prelude_function(require ? "require_handler" : "expect_handler", u_, src)));
    const auto              at{builtin_name_location(src)};
    std::vector<gir::value> call_args;
    call_args.emplace_back(checked);
    call_args.emplace_back(constant(pool.intern(u_.source->path.string(), text_type), text_type));
    call_args.emplace_back(constant(pool.intern(u64{at.line}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(u64{at.column}, u32_type), u32_type));
    call_args.emplace_back(message);

    auto&      result_type{builtin(require ? gs::type_kind::VOID_ : gs::type_kind::BOOL)};
    const auto result{builder_.emit_builtin_call(
        require ? "@require" : "@expect", std::move(call_args), result_type)};
    if (require || !result) { return void_value(); }
    return gir::value{*result, result_type};
}

auto analyzer::runtime_assert(gsl::span<const ref> args, const source& src, bool verify)
    -> result<operand> {
    auto&      ctx{comp_.ctx()};
    const auto cond{TRY(resolve_value(args.front(), src))};

    // A condition known to hold needs no check, and only `@verify` checks without safety
    if (const auto known{TRY(speculated_value(args.front(), src))}) {
        const auto held{comp_.constants().value_at(*known).data.as_opt<bool>()};
        if (held && *held) { return void_value(); }
    }

    if (!verify && !runtime_safety()) { return void_value(); }
    const auto text{string_literal(mod(), args, 1)};
    if (args.size() > 1 && !text) {
        return comp_.unsupported(u_, src, "an `@assert` message that isn't a literal");
    }

    // The handler is emitted for the call codegen lowers the builtin to
    DISCARD(TRY(comp_.prelude_function(verify ? PANIC_HANDLER : "assert_handler", u_, src)));
    const auto checked{TRY(condition(cond, src))};

    auto&      pool{comp_.constants()};
    auto&      u32_type{ctx.get_int(32, false)};
    auto&      text_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    const auto at{builtin_name_location(src)};
    const auto message{
        text.value_or(verify ? std::string_view{"verification failed"} : "assertion failed")};
    std::vector<gir::value> call_args;
    call_args.emplace_back(checked);
    call_args.emplace_back(constant(pool.intern(u_.source->path.string(), text_type), text_type));
    call_args.emplace_back(constant(pool.intern(u64{at.line}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(u64{at.column}, u32_type), u32_type));
    call_args.emplace_back(constant(pool.intern(std::string{message}, text_type), text_type));
    DISCARD(builder_.emit_builtin_call(
        verify ? "@verify" : "@assert", std::move(call_args), builtin(gs::type_kind::VOID_)));
    return void_value();
}

auto analyzer::descriptor(gsl::span<const ref> args,
                          std::string_view     builtin_name,
                          std::string_view     info_name,
                          const source&        src) -> result<ip::index> {
    if (args.size() != 1) { return comp_.unsupported(u_, src, "a type builtin's arity"); }
    auto&      info{*TRY(comp_.prelude_type(info_name, u_, src))};
    const auto arg{TRY(resolve_value(args.front(), src))};

    // An untyped `.{ ... }` descriptor is built at compile time, even in a runtime body
    const auto typed{[&] {
        const auto scope{comptime_scope()};
        return coerce_to(arg, info, src, {.what = use_site::kind::STORE});
    }()};
    if (!typed && typed.error() != stop::RUNTIME) { return stdx::err{typed.error()}; }
    const auto known{typed ? comptime_value(*typed) : stdx::option<ip::index>{}};
    if (!known) {
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            fmt::format("'{}' expects a compile-time-known descriptor argument", builtin_name));
    }

    return *known;
}

auto analyzer::builtin_int_type(gsl::span<const ref> args, const source& src) -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Int", "IntInfo", src))};
    auto&      pool{comp_.constants()};
    const auto bits_field{TRY(comptime_field(desc, "bits", src))};
    const auto sign_field{TRY(comptime_field(desc, "signedness", src))};
    const auto bits_index{comptime_value(bits_field)};
    const auto sign_index{comptime_value(sign_field)};
    const auto bits{bits_index ? int_of(pool.value_at(*bits_index)) : stdx::none};
    const auto sign{sign_index ? variant_name(*sign_index) : stdx::none};
    if (!bits || !sign) { return comp_.unsupported(u_, src, "an `@Int` descriptor it can't read"); }

    const bool is_signed{*sign == "signed"};
    const auto width{bits->to_u128()};
    if (!width || *width > 65'535 || (is_signed && *width == 0)) {
        return comp_.report(
            u_,
            src,
            gs::error::LITERAL_OUT_OF_RANGE,
            fmt::format("'@Int' has no {} {}-bit integer type; widths are {}..65535",
                        *sign,
                        bits->to_string(),
                        is_signed ? 1 : 0));
    }

    return interned_type(comp_.ctx().get_int(static_cast<u16>(*width), is_signed));
}

auto analyzer::builtin_float_type(gsl::span<const ref> args, const source& src) -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Float", "FloatInfo", src))};
    const auto bits_field{TRY(comptime_field(desc, "bits", src))};
    const auto bits_index{comptime_value(bits_field)};
    const auto bits{bits_index ? int_of(comp_.constants().value_at(*bits_index)) : stdx::none};
    if (!bits) { return comp_.unsupported(u_, src, "a `@Float` descriptor it can't read"); }

    const auto width{bits->to_u128().value_or(0)};
    const auto kind{[&] -> stdx::option<gs::type_kind> {
        switch (static_cast<u64>(width)) {
        case 16:  return gs::type_kind::F16;
        case 32:  return gs::type_kind::F32;
        case 64:  return gs::type_kind::F64;
        case 80:  return gs::type_kind::F80;
        case 128: return gs::type_kind::F128;
        default:  return stdx::none;
        }
    }()};
    if (!kind || width > 128) {
        return comp_.report(
            u_,
            src,
            gs::error::LITERAL_OUT_OF_RANGE,
            fmt::format("'@Float' has no {}-bit floating-point type", bits->to_string()));
    }

    return interned_type(builtin(*kind));
}

auto analyzer::builtin_shaped_type(syntax::builtin_t    which,
                                   gsl::span<const ref> args,
                                   const source&        src) -> result<operand> {
    using syntax::builtin_t;
    const auto name{syntax::builtin_name(which)};
    const auto info{which == builtin_t::SLICE   ? "SliceInfo"
                    : which == builtin_t::ARRAY ? "ArrayInfo"
                                                : "PointerInfo"};
    const auto desc{TRY(descriptor(args, name, info, src))};
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto missing{[&](std::string_view field) -> stdx::err<stop> {
        return comp_.report(
            u_,
            src,
            gs::error::COMPTIME_EVALUATION_FAILED,
            fmt::format(
                "'{}': descriptor is missing a compile-time-known '{}' field", name, field));
    }};
    const auto flag{[&](std::string_view field) -> result<bool> {
        const auto known{TRY(known_field(desc, field, src))};
        const auto value{known ? pool.value_at(*known).data.as_opt<bool>() : stdx::none};
        if (!value) { return missing(field); }
        return *value;
    }};

    const auto child_index{TRY(known_field(desc, "child", src))};
    if (!child_index || !pool.is_type(*child_index)) { return missing("child"); }
    auto& child{pool.type_at(*child_index)};
    auto  mods{TRY(flag("is_mut")) ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT};
    if (TRY(flag("is_volatile"))) { mods |= gs::types::mut::VOLATILE; }
    switch (which) {
    case builtin_t::POINTER:   return interned_type(ctx.get_pointer(mods, child));
    case builtin_t::REFERENCE: return interned_type(ctx.get_reference(mods, child));
    case builtin_t::SLICE: return interned_type(ctx.get_slice(mods, TRY(flag("sentinel")), child));
    default:               break;
    }

    const auto len_index{TRY(known_field(desc, "len", src))};
    const auto len{len_index ? int_of(pool.value_at(*len_index)) : stdx::none};
    const auto small{len ? len->to_u128() : stdx::none};
    if (!small) { return missing("len"); }
    return interned_type(
        ctx.get_array(mods, TRY(flag("sentinel")), static_cast<usize>(*small), child));
}

auto analyzer::builtin_fn_type(gsl::span<const ref> args, const source& src) -> result<operand> {
    const auto desc{TRY(descriptor(args, "@Fn", "FnInfo", src))};
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto fail{[&](gs::error code, std::string message) -> stdx::err<stop> {
        return comp_.report(u_, src, code, std::move(message));
    }};
    const auto missing{[&](std::string_view field) {
        return fail(
            gs::error::COMPTIME_EVALUATION_FAILED,
            fmt::format("'@Fn': descriptor is missing a compile-time-known '{}' field", field));
    }};
    const auto flag{[&](std::string_view field) -> result<bool> {
        const auto known{TRY(known_field(desc, field, src))};
        const auto value{known ? pool.value_at(*known).data.as_opt<bool>() : stdx::none};
        if (!value) { return missing(field); }
        return *value;
    }};

    const auto params_index{TRY(known_field(desc, "params", src))};
    const auto params{params_index ? pool.value_at(*params_index).data.as_opt<ip::aggregate_value>()
                                   : stdx::none};
    if (!params) { return missing("params"); }
    const std::vector<stdx::option<ip::index>> elements{params->elements};
    const auto ret_index{TRY(known_field(desc, "return_type", src))};
    if (!ret_index || !pool.is_type(*ret_index)) { return missing("return_type"); }
    auto&      ret{pool.type_at(*ret_index)};
    const bool variadic{TRY(flag("variadic"))};
    const bool has_self{TRY(flag("has_self"))};
    const bool erased{TRY(flag("erased"))};
    const auto conv_index{TRY(known_field(desc, "callconv", src))};
    const auto conv_name{conv_index ? variant_name(*conv_index) : stdx::none};
    if (!conv_name) { return missing("callconv"); }
    const auto conv{ast::calling_convention_from_name(*conv_name)};
    if (!conv) {
        return fail(gs::error::TYPE_MISMATCH,
                    fmt::format("'@Fn': unknown calling convention '{}'", *conv_name));
    }

    if (erased && has_self) {
        return fail(gs::error::TYPE_MISMATCH,
                    "'@Fn': a method-shaped (`has_self`) type is never erased; set "
                    "`.erased = false`");
    }

    if (erased && *conv != ast::calling_convention::C) {
        return fail(gs::error::CALLCONV_REQUIRES_EXTERN_FN,
                    "`callconv(...)` only applies to a thin `extern fn(...)` type; an erased "
                    "`fn(...)` callable has no C calling convention");
    }

    auto param_types{ctx.pool.get_many_unsafe(elements.size())};
    for (usize i{0}; i < elements.size(); ++i) {
        if (!elements[i] || !pool.is_type(*elements[i])) {
            return fail(gs::error::COMPTIME_EVALUATION_FAILED,
                        "'@Fn': every element of 'params' must be a compile-time-known type");
        }

        param_types[i] = &pool.type_at(*elements[i]);
    }

    return interned_type(
        ctx.get_function_like(param_types, ret, has_self, variadic, *conv, erased));
}

auto analyzer::builtin_target(syntax::builtin_t which, const source& src) -> result<operand> {
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto facts{codegen::target_facts::resolve(ctx.target_opts.triple_str)};
    if (which == syntax::builtin_t::TARGET_PTR_BITS) {
        return operand{
            intern_int(pool, big_int{u128{facts.ptr_bits}}, builtin(gs::type_kind::COMPTIME_INT))};
    }

    if (which == syntax::builtin_t::TARGET_TRIPLE) {
        auto& text{ctx.get_slice(gs::types::mut::CONSTANT, true, ctx.get_int(8, false))};
        return operand{
            pool.intern(codegen::resolve_target_triple(ctx.target_opts.triple_str).str(), text)};
    }

    const auto [enum_name, member]{[&] -> std::pair<std::string_view, std::string_view> {
        switch (which) {
        case syntax::builtin_t::TARGET_OS:     return {"Os", facts.os};
        case syntax::builtin_t::TARGET_ARCH:   return {"Arch", facts.arch};
        case syntax::builtin_t::TARGET_ABI:    return {"Abi", facts.abi};
        case syntax::builtin_t::TARGET_ENDIAN: return {"Endian", facts.endian};
        case syntax::builtin_t::OPTIMIZE_MODE:
            return {"OptimizeMode", gs::optimize_mode_name(ctx.build_mode)};
        default: return {"Family", facts.family};
        }
    }()};

    auto&       enum_type{*TRY(comp_.prelude_type(enum_name, u_, src))};
    const auto& en{enum_type.get_data().as<gs::types::enum_t>()};
    const bool  listed{std::ranges::contains(en.variant_names, member)};
    if (!listed) {
        return comp_.unsupported(
            u_, src, fmt::format("a target the `{}` enum doesn't list", enum_name));
    }

    return enum_constant(enum_type, member, src);
}

auto analyzer::builtin_vector_type(gsl::span<const ref> args, const source& src)
    -> result<operand> {
    auto&      ctx{comp_.ctx()};
    auto&      pool{comp_.constants()};
    const auto arg_src{[&](usize i) {
        return i < args.size()
                   ? as_inst(args[i]).transform([&](inst_id a) { return src_of(a); }).value_or(src)
                   : src;
    }};
    const auto unknown{[&](usize arg, std::string_view what) -> result<operand> {
        return comp_.report(u_,
                            arg_src(arg),
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            fmt::format("'@Vector' needs a compile-time-known {}", what));
    }};

    stdx::option<big_int>   len;
    stdx::option<gs::type&> lane;
    if (args.size() == 1) {
        const auto desc{TRY(descriptor(args, "@Vector", "VectorInfo", src))};
        const auto len_index{TRY(known_field(desc, "len", src))};
        const auto child_index{TRY(known_field(desc, "child", src))};
        if (len_index) { len = int_of(pool.value_at(*len_index)); }
        if (!len) { return unknown(0, "'len' field"); }
        if (!child_index || !pool.is_type(*child_index)) { return unknown(0, "'child' field"); }
        lane.emplace(pool.type_at(*child_index));
    } else if (args.size() == 2) {
        const auto count{TRY(resolve_value(args[0], src))};
        const auto count_index{comptime_value(count)};
        if (count_index) { len = int_of(pool.value_at(*count_index)); }
        if (!len) { return unknown(0, "lane count"); }
        lane.emplace(*TRY(resolve_type(args[1], arg_src(1))));
    } else {
        return comp_.unsupported(u_, src, "a `@Vector` arity");
    }

    const auto count{len->to_i128()};
    if (!count || *count < 1 || *count > i128{gs::MAX_VECTOR_LEN}) {
        const auto shown{count ? std::clamp<i128>(*count, 0, i128{gs::MAX_VECTOR_LEN} + 1)
                               : i128{0}};
        return comp_.report(u_,
                            arg_src(0),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'@Vector' has {} lanes; it needs 1..{}",
                                        static_cast<i64>(shown),
                                        gs::MAX_VECTOR_LEN));
    }

    auto& lane_type{ctx.as_lane(*lane)};
    if (const auto why{gs::vector_lane_rejection(lane_type)}) {
        return comp_.report(u_,
                            arg_src(args.size() == 1 ? 0 : 1),
                            gs::error::TYPE_MISMATCH,
                            fmt::format("'{}' can't be a '@Vector' lane: {}",
                                        ctx.type_display_name(lane_type),
                                        *why));
    }

    return interned_type(
        ctx.get_vector(gs::types::mut::CONSTANT, static_cast<u32>(*count), lane_type));
}

auto analyzer::builtin_panic(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (is_comptime()) { return comp_.unsupported(u_, src, "a `@panic` at compile time"); }

    // The message is a string known at compile time, a literal or a value
    std::string message{"panic"};
    if (const auto text{string_literal(mod(), args, 0)}) {
        message = std::string{*text};
    } else if (!args.empty()) {
        const auto value{TRY(resolve_value(args.front(), src))};
        const auto known{comptime_value(value)};
        const auto text_value{known ? comp_.constants().value_at(*known).data.as_opt<std::string>()
                                    : stdx::none};
        if (!text_value) { return comp_.unsupported(u_, src, "a `@panic` message it can't read"); }
        message = *text_value;
    }

    TRY(emit_panic(message, src));
    return void_value();
}

auto analyzer::builtin_implements(gsl::span<const ref> args, const source& src) -> result<operand> {
    if (args.size() != 2) { return comp_.unsupported(u_, src, "an `@implements` arity"); }
    auto& target{*TRY(resolve_type(args[0], src))};
    auto& iface{*TRY(resolve_type(args[1], src))};
    auto& bool_type{builtin(gs::type_kind::BOOL)};
    if (iface.get_kind() != gs::type_kind::INTERFACE) {
        return operand{comp_.constants().intern(false, bool_type)};
    }

    const auto impls{TRY(comp_.impls_of(target))};
    const bool found{std::ranges::any_of(
        impls, [&](const auto applied) { return applied->interface == iface; })};
    return operand{comp_.constants().intern(found, bool_type)};
}

} // namespace ghoti::uir::sema
