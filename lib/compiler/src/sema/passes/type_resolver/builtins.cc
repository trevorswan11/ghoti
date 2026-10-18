#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
#include <llvm/IR/InlineAsm.h>
#include <llvm/TargetParser/Triple.h>
#include <stdx/assert.hh>
#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/ast/format.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/traits.hh"
#include "compiler/codegen/c_abi.hh"
#include "compiler/codegen/intrinsics.hh"
#include "compiler/codegen/target.hh"
#include "compiler/gir/const_eval.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/sema/peer_type.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/syntax/token_type.hh"
#include "internal.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

using namespace resolver_detail;

auto type_resolver::visit(ast::node_id id, const ast::asm_expr& node) -> void {
    PROFILE_FUNCTION();

    // Resolve every string literal (template, constraints, clobbers) and operand expression so
    // that all child nodes carry a resolved type for the emitter and later passes.
    resolve(node.tmpl);
    for (const auto& op : node.outputs) { resolve(op.constraint); }
    for (const auto& op : node.inputs) { resolve(op.constraint); }
    for (const auto& clobber : node.clobbers) { resolve(clobber); }
    for (const auto& op : node.inputs) {
        // Only an output may be discarded. An input has to pass something in
        if (!op.value) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "An asm input operand needs a value; only outputs may be `_`",
                                 error::ILLEGAL_INLINE_ASM,
                                 resolving_.ast.location_of(op.constraint)));
        }
        TRY_RESOLVE(*op.value);
        if (operand_nature(*op.value) == operand_nature_t::TYPE) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 "An asm input operand must be a value, not a type",
                                 error::ILLEGAL_INLINE_ASM,
                                 resolving_.ast.location_of(*op.value)));
        }
    }

    // Each operand constraint must be exactly one well-formed LLVM constraint of its direction
    const auto reject_constraint{[&](ast::string_handle constraint, bool is_output) -> bool {
        const auto text{resolving_.ast.get_as<ast::string_expr>(constraint).value};
        const auto parsed{llvm::InlineAsm::ParseConstraints(text)};
        const bool well_formed{parsed.size() == 1 &&
                               (parsed.front().Type == llvm::InlineAsm::isOutput) == is_output &&
                               parsed.front().Type != llvm::InlineAsm::isClobber};
        if (well_formed) { return false; }
        last_type_.emplace(ctx_.poison_node(
            resolving_,
            id,
            fmt::format("Invalid asm {} constraint \"{}\"", is_output ? "output" : "input", text),
            error::ILLEGAL_INLINE_ASM,
            resolving_.ast.location_of(constraint)));
        return true;
    }};
    for (const auto& op : node.outputs) {
        if (reject_constraint(op.constraint, true)) { return; }
    }
    for (const auto& op : node.inputs) {
        if (reject_constraint(op.constraint, false)) { return; }
    }

    usize bound_output_count{0};
    usize result_slot_count{0};
    for (const auto& op : node.outputs) {
        if (!op.value) {
            ++result_slot_count;
            continue;
        }
        {
            const mutating_context_guard g{in_mutating_context_};
            TRY_RESOLVE(*op.value);
        }
        ++bound_output_count;

        // Outputs must name something assignable. Deep mutability is a pass-3 concern.
        const auto kind{(*op.value)->get_kind()};
        const auto assignable{kind == ast::node_kind::IDENTIFIER_EXPRESSION ||
                              kind == ast::node_kind::DOT_EXPRESSION ||
                              kind == ast::node_kind::INDEX_EXPRESSION ||
                              kind == ast::node_kind::DEREFERENCE_EXPRESSION ||
                              kind == ast::node_kind::IMPLICIT_ACCESS_EXPRESSION};
        if (!assignable) {
            return last_type_.emplace(
                ctx_.poison_node(resolving_,
                                 id,
                                 std::string{"An asm output operand must be an assignable lvalue"},
                                 error::ILLEGAL_INLINE_ASM,
                                 resolving_.ast.location_of(*op.value)));
        }
    }

    stdx::option<type&> declared_result_type;
    if (node.result_type) {
        resolve(*node.result_type);
        declared_result_type.emplace(*last_type_);
    }

    const auto fail = [&](std::string msg) -> void {
        last_type_.emplace(ctx_.poison_node(resolving_,
                                            id,
                                            std::move(msg),
                                            error::ILLEGAL_INLINE_ASM,
                                            resolving_.ast.location_of(id)));
    };

    const bool has_volatile{node.has_option(ast::asm_expr::option::VOLATILE)};
    const bool has_noreturn{node.has_option(ast::asm_expr::option::NORETURN)};
    const bool has_intel{node.has_option(ast::asm_expr::option::INTEL)};
    const bool has_att{node.has_option(ast::asm_expr::option::ATT)};

    for (const auto opt : stdx::enum_range<ast::asm_expr::option>()) {
        if (std::ranges::count(node.options, opt) > 1) { return fail("Duplicate asm option"); }
    }
    if (has_intel && has_att) {
        return fail("The asm options 'intel' and 'att' are mutually exclusive");
    }

    // `volatile` is never implied: a no-output asm must state it or it has no observable effect.
    if (node.outputs.empty() && !has_volatile) {
        return fail("An asm block with no output operands must be marked `volatile`");
    }

    if (result_slot_count > 1) {
        return fail("An asm block may have at most one `_` result operand");
    }
    if (result_slot_count == 1 && !node.result_type) {
        return fail("An asm block with a `_` result operand needs an explicit result type, "
                    "e.g. `asm u32 { ... }`");
    }
    if (result_slot_count == 1 && node.outputs.size() != 1) {
        return fail("An asm block with a `_` result operand may not bind any other outputs");
    }
    if (result_slot_count == 0 && node.result_type) {
        return fail("An asm result type is only meaningful alongside a `_` result operand");
    }
    if (has_noreturn && (bound_output_count > 0 || result_slot_count > 0)) {
        return fail("A `noreturn` asm block cannot bind any output operands");
    }

    // Template placeholder bounds: `%N` must index an operand
    const auto& tmpl_str{resolving_.ast.get_as<ast::string_expr>(node.tmpl).value};
    const usize operand_count{node.outputs.size() + node.inputs.size()};
    for (usize i{0}; i + 1 < tmpl_str.size(); ++i) {
        if (tmpl_str[i] != '%') { continue; }
        if (tmpl_str[i + 1] == '%') {
            ++i;
            continue;
        }
        if (!std::isdigit(tmpl_str[i + 1])) { continue; }
        usize idx{0};
        usize j{i + 1};
        for (; j < tmpl_str.size() && std::isdigit(tmpl_str[j]); ++j) {
            idx = (idx * 10) + static_cast<usize>(tmpl_str[j] - '0');
        }
        i = j - 1;
        if (idx >= operand_count) {
            return fail(fmt::format("asm template references operand %{} but {} operand(s) are "
                                    "given (outputs are numbered first, then inputs)",
                                    idx,
                                    operand_count));
        }
    }

    // A `_` result slot makes `asm` an rvalue of the declared type
    type& node_type{result_slot_count == 1 ? *declared_result_type
                                           : ctx_.get_builtin_resolved_type(type_kind::VOID_)};
    resolving_.set_sema_type(id, node_type);
    last_type_.emplace(node_type);
}

namespace {

// Bit width of a scalar type usable as an atomic operand, or none if the kind isn't supported.
[[nodiscard]] auto atomic_operand_width(const type& t, u32 ptr_bits) noexcept -> stdx::option<u32> {
    switch (t.get_kind()) {
    case type_kind::INT:     return u32{int_width(t)};
    case type_kind::ISIZE:
    case type_kind::USIZE:
    case type_kind::POINTER: return ptr_bits;
    case type_kind::BOOL:    return u32{8};
    case type_kind::F16:
    case type_kind::F32:
    case type_kind::F64:
    case type_kind::F80:
    case type_kind::F128:    return u32{float_bits(t.get_kind())};
    default:                 return stdx::none;
    }
}

[[nodiscard]] auto is_valid_memory_order_name(std::string_view name) noexcept -> bool {
    return name == "relaxed" || name == "acquire" || name == "release" || name == "acq_rel" ||
           name == "seq_cst";
}

// A total order over `MemoryOrder` matching C++11 [atomics.types.operations]
[[nodiscard]] auto memory_order_rank(std::string_view name) noexcept -> i32 {
    if (name == "relaxed") { return 0; }
    if (name == "acquire" || name == "release") { return 1; }
    if (name == "acq_rel") { return 2; }
    if (name == "seq_cst") { return 3; }
    return -1;
}

[[nodiscard]] auto is_valid_atomic_rmw_op_name(std::string_view name) noexcept -> bool {
    static constexpr std::array names{
        "xchg", "add", "sub", "band", "nand", "bor", "bxor", "max", "min", "umax", "umin"};
    return std::ranges::contains(names, name);
}

[[nodiscard]] auto integer_effective_bits(const type& t, u32 ptr_bits) noexcept -> u32 {
    switch (t.get_kind()) {
    case type_kind::INT:   return u32{int_width(t)};
    case type_kind::ISIZE:
    case type_kind::USIZE: return ptr_bits;
    default:               return 0;
    }
}

} // namespace

namespace {

enum class builtin_family : u8 {
    CAST,
    REFLECTION,
    TYPE_CONSTRUCTION,
    TARGET,
    MATH,
    MEMORY,
    ATOMIC,
    C_VARIADIC,
    PROGRAM,
};

[[nodiscard]] auto family_of(syntax::builtin_t builtin) -> builtin_family {
    switch (builtin) {
    case syntax::builtin_t::ALIGN_CAST:
    case syntax::builtin_t::PTR_CAST:
    case syntax::builtin_t::BIT_CAST:
    case syntax::builtin_t::AS:
    case syntax::builtin_t::INT_CAST:
    case syntax::builtin_t::TRUNCATE:
    case syntax::builtin_t::BOOL_FROM_INT:
    case syntax::builtin_t::INT_FROM_BOOL:
    case syntax::builtin_t::BACKING_INT:
    case syntax::builtin_t::FROM_BACKING_INT:
    case syntax::builtin_t::INT_FROM_FLOAT:
    case syntax::builtin_t::FLOAT_FROM_INT:
    case syntax::builtin_t::FLOAT_CAST:
    case syntax::builtin_t::DYN_CAST:
    case syntax::builtin_t::CONST_CAST:
    case syntax::builtin_t::VOLATILE_CAST:
    case syntax::builtin_t::INT_FROM_PTR:
    case syntax::builtin_t::PTR_FROM_INT:
    case syntax::builtin_t::SLICE_FROM_PTR:           return builtin_family::CAST;
    case syntax::builtin_t::ALIGN_OF:
    case syntax::builtin_t::SIZE_OF:
    case syntax::builtin_t::BIT_SIZE_OF:
    case syntax::builtin_t::TYPE_OF:
    case syntax::builtin_t::THIS:
    case syntax::builtin_t::FN_CTX:
    case syntax::builtin_t::FIELD_PARENT_PTR:
    case syntax::builtin_t::TAG_NAME:
    case syntax::builtin_t::IMPLEMENTS:
    case syntax::builtin_t::TYPE_NAME:
    case syntax::builtin_t::TYPE_INFO:
    case syntax::builtin_t::HAS_FIELD:
    case syntax::builtin_t::FIELD_TYPE:
    case syntax::builtin_t::FIELD_DEFAULT:
    case syntax::builtin_t::FIELD:                    return builtin_family::REFLECTION;
    case syntax::builtin_t::INT:
    case syntax::builtin_t::FLOAT:
    case syntax::builtin_t::POINTER:
    case syntax::builtin_t::REFERENCE:
    case syntax::builtin_t::SLICE:
    case syntax::builtin_t::ARRAY:
    case syntax::builtin_t::FN:
    case syntax::builtin_t::ENUM:
    case syntax::builtin_t::STRUCT:
    case syntax::builtin_t::UNION:                    return builtin_family::TYPE_CONSTRUCTION;
    case syntax::builtin_t::TARGET_TRIPLE:
    case syntax::builtin_t::TARGET_OS:
    case syntax::builtin_t::TARGET_ARCH:
    case syntax::builtin_t::TARGET_ABI:
    case syntax::builtin_t::TARGET_FAMILY:
    case syntax::builtin_t::OPTIMIZE_MODE:
    case syntax::builtin_t::RUNTIME_SAFETY:
    case syntax::builtin_t::BRANCH_HINT:
    case syntax::builtin_t::SET_RUNTIME_SAFETY:
    case syntax::builtin_t::TARGET_ENDIAN:
    case syntax::builtin_t::TARGET_PTR_BITS:
    case syntax::builtin_t::SET_EVAL_RECURSION_LIMIT:
    case syntax::builtin_t::SET_EVAL_UNROLL_LIMIT:
    case syntax::builtin_t::EXPORT:
    case syntax::builtin_t::RETURN_ADDRESS:           return builtin_family::TARGET;
    case syntax::builtin_t::CLZ:
    case syntax::builtin_t::CTZ:
    case syntax::builtin_t::POPCOUNT:
    case syntax::builtin_t::SQRT:
    case syntax::builtin_t::SIN:
    case syntax::builtin_t::COS:
    case syntax::builtin_t::TAN:
    case syntax::builtin_t::EXP:
    case syntax::builtin_t::EXP2:
    case syntax::builtin_t::LOG:
    case syntax::builtin_t::LOG2:
    case syntax::builtin_t::LOG10:
    case syntax::builtin_t::FLOOR:
    case syntax::builtin_t::CEIL:
    case syntax::builtin_t::ABS:
    case syntax::builtin_t::MUL_ADD:
    case syntax::builtin_t::MIN:
    case syntax::builtin_t::MAX:
    case syntax::builtin_t::DIV_TRUNC:
    case syntax::builtin_t::DIV_FLOOR:
    case syntax::builtin_t::DIV_EXACT:
    case syntax::builtin_t::REM:
    case syntax::builtin_t::MOD:
    case syntax::builtin_t::ADD_WITH_OVERFLOW:
    case syntax::builtin_t::SUB_WITH_OVERFLOW:
    case syntax::builtin_t::MUL_WITH_OVERFLOW:
    case syntax::builtin_t::SHL_WITH_OVERFLOW:        return builtin_family::MATH;
    case syntax::builtin_t::MEMCPY:
    case syntax::builtin_t::MEMSET:
    case syntax::builtin_t::MEMMOVE:                  return builtin_family::MEMORY;
    case syntax::builtin_t::ATOMIC_LOAD:
    case syntax::builtin_t::ATOMIC_STORE:
    case syntax::builtin_t::ATOMIC_RMW:
    case syntax::builtin_t::CMPXCHG_WEAK:
    case syntax::builtin_t::CMPXCHG_STRONG:
    case syntax::builtin_t::FENCE:                    return builtin_family::ATOMIC;
    case syntax::builtin_t::C_VA_START:
    case syntax::builtin_t::C_VA_COPY:
    case syntax::builtin_t::C_VA_END:
    case syntax::builtin_t::C_VA_ARG:                 return builtin_family::C_VARIADIC;
    case syntax::builtin_t::PANIC:
    case syntax::builtin_t::TRAP:
    case syntax::builtin_t::SKIP:
    case syntax::builtin_t::COMPILE_ERROR:
    case syntax::builtin_t::SRC:
    case syntax::builtin_t::EMBED:
    case syntax::builtin_t::ENTRY_MAIN:
    case syntax::builtin_t::ENTRY_TEST_RUNNER:
    case syntax::builtin_t::TEST_FUNCTIONS:
    case syntax::builtin_t::ENUM_LITERAL:
    case syntax::builtin_t::LLVM_INTRINSIC:
    case syntax::builtin_t::EXPECT:
    case syntax::builtin_t::REQUIRE:
    case syntax::builtin_t::ASSERT:
    case syntax::builtin_t::VERIFY:                   return builtin_family::PROGRAM;
    default: UNREACHABLE("Unimplemented builtin type resolution");
    }
}

} // namespace

auto type_resolver::check_builtin_arity(const ast::call_expr&          call,
                                        const types::builtin_function& builtin,
                                        syntax::builtin_t              builtin_id) const
    -> stdx::result<void, diagnostic> {
    const auto is_expect_or_require{builtin_id == syntax::builtin_t::EXPECT ||
                                    builtin_id == syntax::builtin_t::REQUIRE};
    const auto is_assert_or_verify{builtin_id == syntax::builtin_t::ASSERT ||
                                   builtin_id == syntax::builtin_t::VERIFY};
    const auto is_skip{builtin_id == syntax::builtin_t::SKIP};
    const auto is_inferrable_cast{
        builtin_id == syntax::builtin_t::AS || builtin_id == syntax::builtin_t::INT_CAST ||
        builtin_id == syntax::builtin_t::BIT_CAST || builtin_id == syntax::builtin_t::TRUNCATE ||
        builtin_id == syntax::builtin_t::INT_FROM_BOOL ||
        builtin_id == syntax::builtin_t::FROM_BACKING_INT ||
        builtin_id == syntax::builtin_t::INT_FROM_FLOAT ||
        builtin_id == syntax::builtin_t::FLOAT_FROM_INT ||
        builtin_id == syntax::builtin_t::FLOAT_CAST};
    const auto& params{builtin.params};
    if (is_expect_or_require || is_assert_or_verify || is_inferrable_cast) {
        if (call.arguments.empty() || call.arguments.size() > 2) {
            return make_sema_err(
                fmt::format("Builtin expects 1 or 2 arguments, found {}", call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
    } else if (is_skip) {
        if (call.arguments.size() > 1) {
            return make_sema_err(
                fmt::format("Builtin expects 0 or 1 arguments, found {}", call.arguments.size()),
                error::ARITY_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
    } else if (builtin_id == syntax::builtin_t::LLVM_INTRINSIC) {
        if (call.arguments.size() < 2) {
            return make_sema_err(fmt::format("'@llvmIntrinsic' expects a name and a return type, "
                                             "then the arguments; found {} argument(s)",
                                             call.arguments.size()),
                                 error::ARITY_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
    } else if (builtin_id == syntax::builtin_t::TYPE_OF) {
        if (call.arguments.empty()) {
            return make_sema_err("'@TypeOf' expects at least 1 argument, found 0",
                                 error::ARITY_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
    } else if (call.arguments.size() != params.size()) {
        return make_sema_err(fmt::format("Builtin expects {} arguments, found {}",
                                         params.size(),
                                         call.arguments.size()),
                             error::ARITY_MISMATCH,
                             resolving_.ast.location_of(call.function));
    }
    return {};
}

auto type_resolver::cast_args_of(const ast::call_expr& call, std::string_view name)
    -> stdx::result<cast_args, diagnostic> {
    cast_args args;

    if (call.arguments.size() == 1) {
        const auto implicit_type{implicit_type_stack_.peek()};
        if (!implicit_type || !implicit_type->is_resolved() ||
            implicit_type->get_kind() == type_kind::AUTO ||
            implicit_type->get_kind() == type_kind::COMPTIME_INT ||
            implicit_type->get_kind() == type_kind::COMPTIME_FLOAT) {
            return make_sema_err(
                fmt::format(
                    "cannot infer the target type of '{}' here; write '{}(T, x)'", name, name),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
        if (implicit_type->is_poison()) { return args; }
        args.target.emplace(*implicit_type);
        args.target_loc = resolving_.ast.location_of(call.function);
        args.operand.emplace(call.arguments[0]);
    } else {
        // A `T: type` parameter is `type`-kinded before it is instantiated
        const auto& target_arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (!call_arg_denotes_type(call.arguments[0]) && !target_arg_type.is_poison() &&
            target_arg_type.get_kind() != type_kind::TYPE) {
            return make_sema_err(
                fmt::format("'{}' expects a target type as its first argument", name),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        args.target.emplace(denoted_type(*get_resolved_call_arg_type(call.arguments[0])));
        args.target_loc = get_call_arg_location(call.arguments[0]);
        args.operand.emplace(call.arguments[1]);
    }
    if (call_arg_denotes_type(*args.operand)) {
        return make_sema_err(
            fmt::format("'{}' expects a value to convert, but was given a type", name),
            error::TYPE_USED_AS_VALUE,
            get_call_arg_location(*args.operand));
    }
    if (get_resolved_call_arg_type(*args.operand)->get_kind() == type_kind::UNDEFINED) {
        return make_sema_err(
            fmt::format("'{}' has no value to convert in 'undefined'; assign 'undefined' "
                        "directly instead",
                        name),
            error::TYPE_MISMATCH,
            get_call_arg_location(*args.operand));
    }
    return args;
}

// Casts and conversions: `@as`, `@intCast`, `@ptrCast`, and the like
template <ast::IndexableID ID>
auto type_resolver::resolve_cast_builtin(ID, builtin_site& site) -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::ALIGN_CAST:
    case syntax::builtin_t::PTR_CAST:   {
        const auto name{builtin_id == syntax::builtin_t::ALIGN_CAST ? "@alignCast" : "@ptrCast"};
        auto&      target{*get_resolved_call_arg_type(call.arguments[0])};
        auto&      operand{*get_resolved_call_arg_type(call.arguments[1])};
        if (call_arg_denotes_type(call.arguments[1])) {
            return make_sema_err(
                fmt::format("'{}' expects a pointer value to cast, but was given a type", name),
                error::TYPE_USED_AS_VALUE,
                get_call_arg_location(call.arguments[1]));
        }
        if (!target.is_poison() && target.get_kind() != type_kind::POINTER) {
            return make_sema_err(fmt::format("'{}' target type must be a pointer; found '{}'",
                                             name,
                                             ctx_.type_display_name(target)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        if (!operand.is_poison() && operand.get_kind() != type_kind::POINTER) {
            const auto hint{operand.get_kind() == type_kind::REFERENCE
                                ? " (did you mean `^` instead of `&`?)"
                                : ""};
            return make_sema_err(fmt::format("'{}' operand must be a pointer; found '{}'{}",
                                             name,
                                             ctx_.type_display_name(operand),
                                             hint),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[1]));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::BIT_CAST: {
        const auto args_res{cast_args_of(call, "@bitCast")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (target.get_kind() == type_kind::COMPTIME_INT ||
            target.get_kind() == type_kind::COMPTIME_FLOAT) {
            return make_sema_err(fmt::format("`@bitCast` target '{}' has no bit layout; use `@as`",
                                             ctx_.type_display_name(target)),
                                 error::TYPE_MISMATCH,
                                 args_res->target_loc);
        }
        auto* src_p{get_call_arg_value_type(*args_res->operand).get()};
        // An untyped float folds as an `f64`, so that is the bit pattern it has to give
        if (src_p->get_kind() == type_kind::COMPTIME_FLOAT) {
            src_p = &ctx_.get_builtin_resolved_type(type_kind::F64);
        }
        auto& src{*src_p};
        // An aggregate only has a bit pattern to reinterpret when it is bit-packed
        const auto is_aggregate{[](const type& t) {
            return t.get_data().is<types::struct_t>() || t.get_data().is<types::union_t>();
        }};
        if (is_aggregate(src) || is_aggregate(target)) {
            const auto src_bits{bit_cast_width(src)};
            const auto target_bits{bit_cast_width(target)};
            if (!src_bits || !target_bits) {
                const auto& offender{src_bits ? target : src};
                return make_sema_err(
                    fmt::format("`@bitCast` needs types with a defined bit layout; '{}' is not a "
                                "packed struct or union",
                                ctx_.type_display_name(offender)),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(call.function));
            }
            if (*src_bits != *target_bits) {
                return make_sema_err(
                    fmt::format("`@bitCast` requires equally sized types; '{}' is {} bit(s) but "
                                "'{}' is {} bit(s)",
                                ctx_.type_display_name(src),
                                *src_bits,
                                ctx_.type_display_name(target),
                                *target_bits),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(call.function));
            }
        } else if (has_fixed_bit_layout(src) && has_fixed_bit_layout(target)) {
            const auto layout{target_layout()};
            const auto src_size{gir::const_eval::type_size_of(src, layout)};
            const auto target_size{gir::const_eval::type_size_of(target, layout)};
            if (src_size != target_size) {
                return make_sema_err(
                    fmt::format("`@bitCast` requires equally sized types; '{}' is {} byte(s) but "
                                "'{}' is {} byte(s)",
                                ctx_.type_display_name(src),
                                src_size,
                                ctx_.type_display_name(target),
                                target_size),
                    error::TYPE_MISMATCH,
                    resolving_.ast.location_of(call.function));
            }
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::AS: {
        const auto args_res{cast_args_of(call, "@as")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }

        if (target.get_kind() == type_kind::BOOL &&
            (is_integer(src.get_kind()) || src.get_kind() == type_kind::COMPTIME_INT ||
             src.get_kind() == type_kind::POINTER)) {
            return make_sema_err("`@as` cannot convert to `bool`; use `@boolFromInt` instead",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        if (src.get_kind() == type_kind::BOOL && is_integer(target.get_kind())) {
            return make_sema_err("`@as` cannot convert from `bool`; use `@intFromBool` instead",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        const bool src_int{is_integer(src.get_kind()) || src.get_kind() == type_kind::COMPTIME_INT};
        if (target.get_kind() == type_kind::ENUM && src_int) {
            return make_sema_err(
                "`@as` cannot convert an integer to an enum; use `@fromBackingInt` instead",
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
        if (src.get_kind() == type_kind::ENUM && is_integer(target.get_kind())) {
            return make_sema_err(
                "`@as` cannot convert an enum to an integer; use `@backingInt` instead",
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
        if (is_integer(target.get_kind()) &&
            (is_float(src.get_kind()) || src.get_kind() == type_kind::COMPTIME_FLOAT)) {
            return make_sema_err(
                "`@as` cannot convert a float to an integer; use `@intFromFloat` instead",
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
        if (is_float(target.get_kind()) && is_float(src.get_kind()) &&
            float_bits(src.get_kind()) > float_bits(target.get_kind())) {
            return make_sema_err(fmt::format("`@as` cannot narrow '{}' to '{}'; use `@floatCast`",
                                             ctx_.type_display_name(src),
                                             ctx_.type_display_name(target)),
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        if (is_float(target.get_kind()) && is_integer(src.get_kind()) &&
            !is_implicit_widenable(src, target)) {
            return make_sema_err(
                fmt::format("`@as` cannot convert '{}' to '{}' exactly; use `@floatFromInt` "
                            "instead",
                            ctx_.type_display_name(src),
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }

        return_type = &target;
        break;
    }
    case syntax::builtin_t::INT_CAST: {
        const auto args_res{cast_args_of(call, "@intCast")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (!is_integer(target.get_kind()) && target.get_kind() != type_kind::COMPTIME_INT) {
            return make_sema_err(
                fmt::format("`@intCast` target must be an integer type; found '{}'",
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        if (!is_integer(src.get_kind()) && src.get_kind() != type_kind::COMPTIME_INT) {
            return make_sema_err(
                fmt::format("`@intCast` operand must be an integer type; found '{}'",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::TRUNCATE: {
        const auto args_res{cast_args_of(call, "@truncate")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (target.get_kind() == type_kind::COMPTIME_INT) {
            return make_sema_err(
                "`@truncate` target 'comptime_int' has no width to truncate to; use `@intCast`",
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        if (!is_integer(target.get_kind())) {
            return make_sema_err(
                fmt::format("`@truncate` target must be an integer type; found '{}'",
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        if (!is_integer(src.get_kind())) {
            return make_sema_err(
                fmt::format("`@truncate` operand must be an integer type; found '{}'",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(*args_res->operand));
        }

        const auto ptr_bits{target_ptr_bits()};
        const auto to_bits{integer_effective_bits(target, ptr_bits)};
        const auto from_bits{integer_effective_bits(src, ptr_bits)};
        if (to_bits == from_bits) {
            return make_sema_err(fmt::format("`@truncate` target type '{}' has the same width as "
                                             "'{}'; use `@bitCast` or `@intCast` instead",
                                             ctx_.type_display_name(target),
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        if (to_bits > from_bits) {
            return make_sema_err(
                fmt::format("`@truncate` target type '{}' is wider than '{}'; use `@as` instead",
                            ctx_.type_display_name(target),
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }

        return_type = &target;
        break;
    }
    case syntax::builtin_t::BOOL_FROM_INT: {
        const auto& arg{call.arguments[0]};
        auto&       src{*get_call_arg_value_type(arg)};
        if (src.is_poison()) { break; }
        if (!is_integer(src.get_kind()) && src.get_kind() != type_kind::COMPTIME_INT &&
            src.get_kind() != type_kind::POINTER) {
            return make_sema_err(
                fmt::format("`@boolFromInt` operand must be an integer or pointer; found '{}'",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(arg));
        }
        return_type = &ctx_.get_builtin_resolved_type(sema::type_kind::BOOL);
        break;
    }
    case syntax::builtin_t::INT_FROM_BOOL: {
        const auto args_res{cast_args_of(call, "@intFromBool")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (!is_integer(target.get_kind())) {
            return make_sema_err(
                fmt::format("`@intFromBool` target must be an integer type; found '{}'",
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        if (src.get_kind() != type_kind::BOOL) {
            return make_sema_err(
                fmt::format("`@intFromBool` operand must be of type 'bool'; found '{}'",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::BACKING_INT: {
        const auto& arg{call.arguments[0]};
        auto&       src{*get_resolved_call_arg_type(arg)};
        if (src.is_poison()) { break; }
        if (call_arg_denotes_type(arg)) {
            return make_sema_err("`@backingInt` expects a value to convert, but was given a type",
                                 error::TYPE_USED_AS_VALUE,
                                 get_call_arg_location(arg));
        }
        const auto backing{ctx_.backing_int_type(src, target_ptr_bits())};
        if (!backing) {
            return make_sema_err(fmt::format("`@backingInt` operand must be an enum, a packed "
                                             "struct or union, or a tagged union; found '{}'",
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(arg));
        }
        return_type = backing.get();
        break;
    }
    case syntax::builtin_t::FROM_BACKING_INT: {
        const auto args_res{cast_args_of(call, "@fromBackingInt")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        type&      target{*args_res->target};
        const auto un{target.get_data().as_opt<types::union_t>()};
        const auto backing{ctx_.backing_int_type(target, target_ptr_bits())};
        if (!backing || (un && !un->is_bit_packed())) {
            return make_sema_err(fmt::format("`@fromBackingInt` target must be an enum or a packed "
                                             "struct or union; found '{}'",
                                             ctx_.type_display_name(target)),
                                 error::TYPE_MISMATCH,
                                 args_res->target_loc);
        }
        auto& src{*get_resolved_call_arg_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        const bool fits{src.get_kind() == type_kind::COMPTIME_INT ||
                        (is_integer(src.get_kind()) && is_assignable(src, *backing))};
        if (!fits) {
            return make_sema_err(fmt::format("`@fromBackingInt` operand must be an integer "
                                             "assignable to '{}', the backing integer of '{}'; "
                                             "found '{}'",
                                             ctx_.type_display_name(*backing),
                                             ctx_.type_display_name(target),
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::INT_FROM_FLOAT: {
        const auto args_res{cast_args_of(call, "@intFromFloat")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (!is_integer(target.get_kind()) && target.get_kind() != type_kind::COMPTIME_INT) {
            return make_sema_err(
                fmt::format("`@intFromFloat` target must be an integer type; found '{}'",
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        if (!is_float(src.get_kind()) && src.get_kind() != type_kind::COMPTIME_FLOAT) {
            return make_sema_err(fmt::format("`@intFromFloat` operand must be a float; found '{}'",
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::FLOAT_FROM_INT: {
        const auto args_res{cast_args_of(call, "@floatFromInt")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (!is_float(target.get_kind()) && target.get_kind() != type_kind::COMPTIME_FLOAT) {
            return make_sema_err(
                fmt::format("`@floatFromInt` target must be a float type; found '{}'",
                            ctx_.type_display_name(target)),
                error::TYPE_MISMATCH,
                args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        if (!is_integer(src.get_kind()) && src.get_kind() != type_kind::COMPTIME_INT) {
            return make_sema_err(
                fmt::format("`@floatFromInt` operand must be an integer; found '{}'",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::FLOAT_CAST: {
        const auto args_res{cast_args_of(call, "@floatCast")};
        if (!args_res) { return make_sema_err(args_res.error()); }
        if (!args_res->target || args_res->target->is_poison()) { break; }
        auto& target{*args_res->target};
        if (!is_float(target.get_kind()) && target.get_kind() != type_kind::COMPTIME_FLOAT) {
            return make_sema_err(fmt::format("`@floatCast` target must be a float type; found '{}'",
                                             ctx_.type_display_name(target)),
                                 error::TYPE_MISMATCH,
                                 args_res->target_loc);
        }
        auto& src{*get_call_arg_value_type(*args_res->operand)};
        if (src.is_poison()) { break; }
        const auto src_kind{src.get_kind()};
        if (is_integer(src_kind)) {
            return make_sema_err(
                fmt::format("`@floatCast` operand must be a float; found '{}'; use "
                            "`@floatFromInt` instead",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(*args_res->operand));
        }
        if (!is_float(src_kind) && src_kind != type_kind::COMPTIME_FLOAT &&
            src_kind != type_kind::COMPTIME_INT) {
            return make_sema_err(fmt::format("`@floatCast` operand must be a float; found '{}'",
                                             ctx_.type_display_name(src)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(*args_res->operand));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::DYN_CAST: {
        // reinterpret `w`'s erased data pointer, unchecked.
        auto& target{*get_resolved_call_arg_type(call.arguments[0])};
        if (target.get_kind() != type_kind::POINTER && target.get_kind() != type_kind::REFERENCE) {
            return make_sema_err(
                "`@dynCast` target must be a pointer or reference type (`^T` / `&T`)",
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        auto&               src{*get_resolved_call_arg_type(call.arguments[1])};
        stdx::option<type&> src_referent{src};
        if (const auto p{src.get_data().as_opt<types::pointer>()}) {
            src_referent.emplace(p->underlying);
        }
        if (const auto r{src.get_data().as_opt<types::reference>()}) {
            src_referent.emplace(r->underlying);
        }
        if (src_referent->get_kind() != type_kind::DYN) {
            return make_sema_err(
                fmt::format("`@dynCast` source must be a `&dyn I` / `^dyn I`; found `{}`",
                            ctx_.type_display_name(src)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[1]));
        }
        return_type = &target;
        break;
    }
    case syntax::builtin_t::CONST_CAST: {
        // Pointer/reference/slice/array is checked since it is an invariant of the cast
        auto&      expr_type{*get_resolved_call_arg_type(call.arguments[0])};
        const auto castable_kind{expr_type.get_kind() == type_kind::POINTER ||
                                 expr_type.get_kind() == type_kind::REFERENCE ||
                                 expr_type.get_kind() == type_kind::SLICE ||
                                 expr_type.get_kind() == type_kind::ARRAY};
        if (castable_kind) {
            return_type = ctx_.pool.with_const(expr_type, false);
        } else {
            return make_sema_err(
                fmt::format("Expected pointer, reference, slice, or array type; found '{}'",
                            ctx_.type_display_name(expr_type)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        break;
    }
    case syntax::builtin_t::VOLATILE_CAST: {
        auto& expr_type{*get_resolved_call_arg_type(call.arguments[0])};
        return_type = ctx_.pool.strip_volatile(expr_type);
        break;
    }
    case syntax::builtin_t::INT_FROM_PTR: {
        auto& operand{*get_resolved_call_arg_type(call.arguments[0])};
        if (!operand.is_poison() && operand.get_kind() != type_kind::POINTER) {
            const auto hint{operand.get_kind() == type_kind::REFERENCE
                                ? " (did you mean `^` instead of `&`?)"
                                : ""};
            return make_sema_err(
                fmt::format("'@intFromPtr' operand must be a pointer; found '{}'{}",
                            ctx_.type_display_name(operand),
                            hint),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        ASSERT(builtin.return_type.get_kind() == type_kind::USIZE);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::PTR_FROM_INT: {
        auto& requested_output{*get_resolved_call_arg_type(call.arguments[0])};
        if (requested_output.get_kind() == type_kind::POINTER) {
            return_type = &requested_output;
            break;
        }
        return make_sema_err(fmt::format("Expected a pointer type; found '{}'",
                                         ctx_.type_display_name(requested_output)),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(call.arguments[0]));
    }
    case syntax::builtin_t::SLICE_FROM_PTR: {
        auto& ptr_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (const auto ptr_data{ptr_type.get_data().as_opt<types::pointer>()}) {
            // The resulting slice isn't null terminated since the pointer gives no guarantee,
            // and mirrors the source pointer's own mutability rather than being const by default
            const auto mutability{ptr_type.is_constant() ? types::mut::CONSTANT
                                                         : types::mut::MUTABLE};
            return_type = &ctx_.get_slice(mutability, false, ptr_data->underlying);
            break;
        }

        return make_sema_err(fmt::format("Expected a pointer-yielding expression; found '{}'",
                                         ctx_.type_display_name(ptr_type)),
                             error::TYPE_MISMATCH,
                             get_call_arg_location(call.arguments[0]));
    }
    default: UNREACHABLE("Builtin is not a cast builtin");
    }
    return {};
}

// Queries about a type or value: sizes, `@TypeOf`, `@typeInfo`, field access
template <ast::IndexableID ID>
auto type_resolver::resolve_reflection_builtin(ID id, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::ALIGN_OF:
    case syntax::builtin_t::SIZE_OF:
    case syntax::builtin_t::BIT_SIZE_OF: {
        ASSERT(builtin.return_type.get_kind() == type_kind::COMPTIME_INT);
        return_type = &builtin.return_type;
        break;
    }
    // The result is an unsigned integer with the minimum number of bits needed to represent the
    // operand type's own bit width
    case syntax::builtin_t::TYPE_OF: {
        ASSERT(builtin.return_type.get_kind() == type_kind::TYPE);
        // Several operands name the type they would meet at
        std::vector<peer_operand> operands;
        bool                      any_deferred{false};
        for (const auto& argument : call.arguments) {
            auto&       operand_type{peer_view(*get_resolved_call_arg_type(argument))};
            const auto& operand_data{operand_type.get_data()};
            any_deferred = any_deferred || operand_data.is<types::deferred_call>() ||
                           operand_data.is<types::deferred_array>();
            operands.emplace_back(&operand_type);
        }
        // An operand whose type isn't known yet defers the whole call until it is
        if (any_deferred) {
            return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &call}];
            return_type->resolve_if<types::deferred_call>(call);
            break;
        }
        const auto peer{peer_type(ctx_, operands)};
        if (!peer) {
            return make_sema_err(
                fmt::format("'@TypeOf': {}", peer_error_message(ctx_, operands, peer.error())),
                error::NO_PEER_TYPE,
                get_call_arg_location(call.arguments[peer.error().second]));
        }
        auto& instance_type{**peer};
        return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, instance_type}];
        return_type->resolve_if<types::meta_type>(instance_type);
        break;
    }
    // @This returns a type as per docs, but it's really a structural type with full determinism
    case syntax::builtin_t::THIS:
        if (const auto user_type{user_type_stack_.peek()}) {
            return_type = user_type.get();
            break;
        }

        return make_sema_err("@This() may only be used inside of structs, unions, and enums",
                             error::TYPE_MISMATCH,
                             resolving_.ast.location_of(id));
    // @fnCtx() returns the enclosing function's own (pre-capture) signature as a callable value
    case syntax::builtin_t::FN_CTX:
        if (open_function_nodes_.empty()) {
            return make_sema_err("@fnCtx() may only be used inside of a function",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(id));
        }
        if (auto& fn_self_type{resolving_.get_sema_type(open_function_nodes_.back())};
            fn_self_type.is_resolved()) {
            return_type = &fn_self_type;
            break;
        }
        return make_sema_err(
            "@fnCtx() needs its enclosing function's return type to be known; give it an "
            "explicit (non-auto) return type",
            error::TYPE_MISMATCH,
            resolving_.ast.location_of(id));
    case syntax::builtin_t::FIELD_PARENT_PTR: {
        auto& parent_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (!parent_type.get_data().is<types::struct_t>()) {
            return make_sema_err(fmt::format("'@fieldParentPtr' expects a struct type; found '{}'",
                                             ctx_.type_display_name(parent_type)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }

        const auto name_expr{call.arguments[1].as_opt<ast::expr_handle>()};
        const auto name_str{name_expr ? resolving_.ast.get_as_opt<ast::string_expr>(*name_expr)
                                      : stdx::none};
        if (!name_str) {
            return make_sema_err("'@fieldParentPtr' expects a string-literal field name",
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[1]));
        }
        const auto& field_table{ctx_.registry.get(parent_type.get_symbol_table_idx())};
        const auto  field_proxy{field_table.get_proxy_opt(name_str->value)};
        const auto  struct_data{parent_type.get_data().as<types::struct_t>()};
        if (!field_proxy || field_proxy->index >= struct_data.fields.size()) {
            return make_sema_err(
                fmt::format("'{}' is not a field of the given struct", name_str->value),
                error::UNKNOWN_FIELD,
                get_call_arg_location(call.arguments[1]));
        }

        auto& field_ptr_type{*get_resolved_call_arg_type(call.arguments[2])};
        if (field_ptr_type.get_kind() != type_kind::POINTER) {
            return make_sema_err(
                fmt::format("'@fieldParentPtr' expects a field pointer; found '{}'",
                            ctx_.type_display_name(field_ptr_type)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[2]));
        }
        const auto mutability{field_ptr_type.is_constant() ? types::mut::CONSTANT
                                                           : types::mut::MUTABLE};
        return_type = &ctx_.get_pointer(mutability, parent_type);
        break;
    }
    case syntax::builtin_t::TAG_NAME: {
        if (call_arg_denotes_type(call.arguments[0])) {
            return make_sema_err(
                "'@tagName' expects an enum or tagged union value, but was given a "
                "type",
                error::TYPE_USED_AS_VALUE,
                get_call_arg_location(call.arguments[0]));
        }
        const auto& operand{*get_resolved_call_arg_type(call.arguments[0])};
        const auto  tagged_union{operand.get_data().as_opt<types::union_t>()};
        const bool  has_tag{operand.get_kind() == type_kind::ENUM ||
                            operand.get_kind() == type_kind::ENUM_LITERAL ||
                            (tagged_union && !tagged_union->is_untagged)};
        if (!has_tag && !operand.is_poison() && !is_generic_type(operand)) {
            return make_sema_err(
                fmt::format("'@tagName' expects an enum or tagged union value; found '{}'",
                            ctx_.type_display_name(operand)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        ASSERT(builtin.return_type.get_kind() == type_kind::SLICE);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::IMPLEMENTS: {
        // `@implements(T | value, I)` -> comptime bool. The value is produced by const-eval.
        DISCARD(get_resolved_call_arg_type(call.arguments[0]));
        DISCARD(get_resolved_call_arg_type(call.arguments[1]));
        ASSERT(builtin.return_type.get_kind() == type_kind::BOOL);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::TYPE_NAME: {
        // Fold any `[N]T` first so the length agrees with the name GIR later materializes
        auto&           arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        gir::const_eval evaluator{ctx_, resolving_};
        auto&           named{evaluator.force_deferred_type(denoted_type(arg_type))};
        const auto      name{ctx_.type_display_name(named)};
        return_type =
            &ctx_.get_array(types::mut::CONSTANT, true, name.size(), ctx_.get_int(8, false));
        break;
    }
    case syntax::builtin_t::TYPE_INFO: {
        DISCARD(get_resolved_call_arg_type(call.arguments[0]));
        if (const auto arg{call.arguments[0].as_opt<ast::expr_handle>()}) {
            gir::const_eval evaluator{ctx_, resolving_};
            const auto      declared{evaluator.declared_fn_attributes(*arg)};
            if (const auto dependent{declared ? declared->dependent : stdx::none}) {
                return make_sema_err(
                    fmt::format("Attribute '{}' depends on the function's parameters, so it has "
                                "a value only within an instantiation",
                                ast::attribute_spec_of(*dependent).name),
                    error::ILLEGAL_ATTRIBUTE,
                    get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &denoted_type(ctx_.get_builtin_type("TypeInfo"));
        break;
    }
    case syntax::builtin_t::HAS_FIELD: {
        // Always folds, so anything that can't is an error here rather than a runtime call later
        const auto& owner_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (!owner_type.is_poison() && !call_arg_denotes_type(call.arguments[0]) &&
            owner_type.get_kind() != type_kind::TYPE) {
            return make_sema_err("@hasField: the first argument must be a type",
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        DISCARD(get_resolved_call_arg_type(call.arguments[1]));
        if (!resolve_field_name_string(call.arguments[1])) {
            return make_sema_err("@hasField: the field name must be a compile-time-known string",
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[1]));
        }
        ASSERT(builtin.return_type.get_kind() == type_kind::BOOL);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::FIELD_TYPE: {
        auto&      arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto&      denoted{denoted_type(arg_type)};
        const auto field{resolve_field_by_name(call.arguments[1], denoted)};
        if (!field) {
            return make_sema_err(
                "@fieldType: unknown field (the name must be a compile-time-known string)",
                error::FIELD_NOT_FOUND,
                get_call_arg_location(call.arguments[1]));
        }
        return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, field->field_type}];
        return_type->resolve_if<types::meta_type>(field->field_type);
        break;
    }
    case syntax::builtin_t::FIELD_DEFAULT: {
        // The field's real default value, statically typed to the field's own type
        auto&      arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        auto&      denoted{denoted_type(arg_type)};
        const auto field{resolve_field_by_name(call.arguments[1], denoted)};
        if (!field) {
            return make_sema_err(
                "@fieldDefault: unknown field (the name must be a compile-time-known string)",
                error::FIELD_NOT_FOUND,
                get_call_arg_location(call.arguments[1]));
        }
        const auto st{denoted.get_data().as_opt<types::struct_t>()};
        if (!st || !st->ast_fields[field->index].default_value) {
            return make_sema_err("@fieldDefault: field has no default value",
                                 error::FIELD_HAS_NO_DEFAULT,
                                 get_call_arg_location(call.arguments[1]));
        }
        return_type = &field->field_type;
        break;
    }
    case syntax::builtin_t::FIELD: {
        auto& arg_type{*get_resolved_call_arg_type(call.arguments[0])};
        if (const auto arg0_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
            gir::const_eval type_check{ctx_, resolving_};
            if (const auto folded{type_check.try_eval(*arg0_h)};
                folded && folded->is<stdx::option<type&>>()) {
                const auto owner_opt{folded->as_opt<stdx::option<type&>>()};
                if (!owner_opt || !*owner_opt) {
                    return make_sema_err("@field: type argument does not denote a concrete type",
                                         error::FIELD_NOT_FOUND,
                                         get_call_arg_location(call.arguments[0]));
                }
                auto&      owner{**owner_opt};
                const auto name{resolve_field_name_string(call.arguments[1])};
                if (!name) {
                    return make_sema_err("@field: the name must be a compile-time-known string",
                                         error::FIELD_NOT_FOUND,
                                         get_call_arg_location(call.arguments[1]));
                }

                const auto not_found = [&] {
                    return make_sema_err(
                        fmt::format(
                            "@field: '{}' has no static data member named '{}' (methods and "
                            "bound access are not supported)",
                            ctx_.type_display_name(owner),
                            *name),
                        error::FIELD_NOT_FOUND,
                        get_call_arg_location(call.arguments[1]));
                };
                if (!owner.has_symbol_table_idx()) { return not_found(); }
                const auto sym{ctx_.registry.get(owner.get_symbol_table_idx()).get_opt(*name)};
                if (!sym || !sym->has_kind() || sym->get_kind() != symbol_kind::VALUE) {
                    return not_found();
                }
                const auto member_node{sym->get_data().as_opt<symbols::node_t>()};
                if (!member_node) { return not_found(); }
                const auto member_decl{resolving_.ast.get_as_opt<ast::decl_stmt>(*member_node)};
                const bool is_fn_member{
                    member_decl && member_decl->value &&
                    resolving_.ast[*member_decl->value].is<ast::function_expr>()};
                if (is_fn_member || !resolving_.has_sema_type(*member_node)) { return not_found(); }
                return_type = &resolving_.get_sema_type(*member_node);
                break;
            }
        }
        auto* denoted{&arg_type};
        if (const auto p{denoted->get_data().as_opt<types::pointer>()}) {
            denoted = &p->underlying;
        } else if (const auto r{denoted->get_data().as_opt<types::reference>()}) {
            denoted = &r->underlying;
        }
        const auto field{resolve_field_by_name(call.arguments[1], *denoted)};
        if (!field) {
            return make_sema_err(
                "@field: unknown field (the name must be a compile-time-known string)",
                error::FIELD_NOT_FOUND,
                get_call_arg_location(call.arguments[1]));
        }
        return_type = &field->field_type;
        break;
    }
    default: UNREACHABLE("Builtin is not a reflection builtin");
    }
    return {};
}

// Builtins that build a type: `@Int`, `@Pointer`, `@Struct`, and the rest
template <ast::IndexableID ID>
auto type_resolver::resolve_type_construction_builtin(ID id, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::INT:
    case syntax::builtin_t::FLOAT:
    case syntax::builtin_t::POINTER:
    case syntax::builtin_t::REFERENCE:
    case syntax::builtin_t::SLICE:
    case syntax::builtin_t::ARRAY:
    case syntax::builtin_t::FN:        {
        const auto builtin_name{syntax::builtin_name(builtin_id)};
        const auto desc{resolve_type_descriptor(call.arguments[0])};
        if (!desc && mentions_unbound_type(call.arguments[0])) {
            return_type = ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, &call}];
            return_type->resolve_if<types::deferred_call>(call);
            break;
        }
        if (!desc) {
            return make_sema_err(
                fmt::format("'{}' expects a compile-time-known descriptor argument", builtin_name),
                error::COMPTIME_EVALUATION_FAILED,
                get_call_arg_location(call.arguments[0]));
        }
        const auto field_err{[&](std::string_view field_name) -> stdx::result<void, diagnostic> {
            return make_sema_err(
                fmt::format("'{}': descriptor is missing a compile-time-known '{}' field",
                            builtin_name,
                            field_name),
                error::COMPTIME_EVALUATION_FAILED,
                get_call_arg_location(call.arguments[0]));
        }};

        const auto wrap_type{[&](type& built) -> type* {
            auto meta{ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, built}]};
            meta->resolve_if<types::meta_type>(built);
            return meta;
        }};

        switch (builtin_id) {
        case syntax::builtin_t::INT: {
            TRY_DESC_FIELD(bits_v, u64, "bits");
            auto signedness_v{read_desc_field<gir::const_enum>(*desc, "signedness")};
            if (!signedness_v) { signedness_v = read_desc_field<gir::const_enum>(*desc, "signed"); }
            if (!signedness_v) { return field_err("signedness"); }
            if (signedness_v->name != "signed" && signedness_v->name != "unsigned") {
                return make_sema_err(
                    fmt::format("'@Int': unknown signedness '{}'", signedness_v->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[0]));
            }
            const bool is_signed{signedness_v->name == "signed"};
            if (*bits_v > 65'535 || (is_signed && *bits_v == 0)) {
                return make_sema_err(
                    fmt::format("'@Int' has no {} {}-bit integer type; widths are {}..65535",
                                signedness_v->name,
                                *bits_v,
                                is_signed ? 1 : 0),
                    error::LITERAL_OUT_OF_RANGE,
                    get_call_arg_location(call.arguments[0]));
            }
            return_type = wrap_type(ctx_.get_int(static_cast<u16>(*bits_v), is_signed));
            break;
        }
        case syntax::builtin_t::FLOAT: {
            TRY_DESC_FIELD(bits_v, u64, "bits");
            type_kind kind{};
            switch (*bits_v) {
            case 16:  kind = type_kind::F16; break;
            case 32:  kind = type_kind::F32; break;
            case 64:  kind = type_kind::F64; break;
            case 80:  kind = type_kind::F80; break;
            case 128: kind = type_kind::F128; break;
            default:
                return make_sema_err(
                    fmt::format("'@Float' has no {}-bit floating-point type", *bits_v),
                    error::LITERAL_OUT_OF_RANGE,
                    get_call_arg_location(call.arguments[0]));
            }
            return_type = wrap_type(ctx_.get_builtin_resolved_type(kind));
            break;
        }
        case syntax::builtin_t::POINTER:
        case syntax::builtin_t::REFERENCE: {
            TRY_DESC_FIELD(child_v, sema::type&, "child");
            TRY_DESC_FIELD(mut_v, bool, "is_mut");
            TRY_DESC_FIELD(vol_v, bool, "is_volatile");
            auto mods{*mut_v ? types::mut::MUTABLE : types::mut::CONSTANT};
            if (*vol_v) { mods |= types::mut::VOLATILE; }
            return_type = wrap_type(builtin_id == syntax::builtin_t::POINTER
                                        ? ctx_.get_pointer(mods, *child_v)
                                        : ctx_.get_reference(mods, *child_v));
            break;
        }
        case syntax::builtin_t::SLICE: {
            TRY_DESC_FIELD(child_v, sema::type&, "child");
            TRY_DESC_FIELD(sentinel_v, bool, "sentinel");
            TRY_DESC_FIELD(mut_v, bool, "is_mut");
            TRY_DESC_FIELD(vol_v, bool, "is_volatile");
            auto mods{*mut_v ? types::mut::MUTABLE : types::mut::CONSTANT};
            if (*vol_v) { mods |= types::mut::VOLATILE; }
            return_type = wrap_type(ctx_.get_slice(mods, *sentinel_v, *child_v));
            break;
        }
        case syntax::builtin_t::ARRAY: {
            TRY_DESC_FIELD(child_v, sema::type&, "child");
            TRY_DESC_FIELD(len_v, u64, "len");
            TRY_DESC_FIELD(sentinel_v, bool, "sentinel");
            TRY_DESC_FIELD(mut_v, bool, "is_mut");
            TRY_DESC_FIELD(vol_v, bool, "is_volatile");
            auto mods{*mut_v ? types::mut::MUTABLE : types::mut::CONSTANT};
            if (*vol_v) { mods |= types::mut::VOLATILE; }
            return_type =
                wrap_type(ctx_.get_array(mods, *sentinel_v, static_cast<usize>(*len_v), *child_v));
            break;
        }
        case syntax::builtin_t::FN: {
            TRY_DESC_FIELD(params_arr, gir::const_array, "params");
            TRY_DESC_FIELD(ret_v, sema::type&, "return_type");
            TRY_DESC_FIELD(variadic_v, bool, "variadic");
            TRY_DESC_FIELD(has_self_v, bool, "has_self");
            TRY_DESC_FIELD(callconv_v, gir::const_enum, "callconv");
            const bool erased{read_desc_field<bool>(*desc, "erased").value_or(true)};

            const auto conv{ast::calling_convention_from_name(callconv_v->name)};
            if (!conv) {
                return make_sema_err(
                    fmt::format("'@Fn': unknown calling convention '{}'", callconv_v->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[0]));
            }
            if (erased && *has_self_v) {
                return make_sema_err("'@Fn': a method-shaped (`has_self`) type is never erased; "
                                     "set `.erased = false`",
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[0]));
            }
            if (erased && *conv != ast::calling_convention::C) {
                return make_sema_err(
                    callconv_requires_extern(get_call_arg_location(call.arguments[0])));
            }

            auto param_types{ctx_.pool.get_many_unsafe(params_arr->elements.size())};
            for (usize i{0}; i < params_arr->elements.size(); ++i) {
                const auto pt{params_arr->elements[i].as_opt<stdx::option<type&>>()};
                if (!pt || !*pt) {
                    return make_sema_err(
                        "'@Fn': every element of 'params' must be a compile-time-known type",
                        error::COMPTIME_EVALUATION_FAILED,
                        get_call_arg_location(call.arguments[0]));
                }
                param_types[i] = pt->get();
            }

            return_type = wrap_type(ctx_.get_function_like(
                param_types, *ret_v, *has_self_v, *variadic_v, *conv, erased));
            break;
        }
        default: ASSERT(false, "unreachable");
        }
        break;
    }
    case syntax::builtin_t::ENUM:
    case syntax::builtin_t::STRUCT:
    case syntax::builtin_t::UNION:  {
        if (const auto existing{resolving_.get_sema_type_opt(id)}) {
            const auto& denoted{denoted_type(*existing)};
            const bool  stale_instantiation{
                for_generic_instantiation_ && reresolve_floor_ &&
                (denoted.is_poison() || (denoted.has_symbol_table_idx() &&
                                         denoted.get_symbol_table_idx() >= *reresolve_floor_))};
            if (!stale_instantiation) {
                return_type = existing.get();
                break;
            }
        }

        const auto builtin_name{syntax::builtin_name(builtin_id)};
        const auto desc{resolve_type_descriptor(call.arguments[0])};
        if (!desc) {
            return make_sema_err(
                fmt::format("'{}' expects a compile-time-known descriptor argument", builtin_name),
                error::COMPTIME_EVALUATION_FAILED,
                get_call_arg_location(call.arguments[0]));
        }

        const auto loc{resolving_.ast.location_of(id)};
        auto synthesized = builtin_id == syntax::builtin_t::ENUM     ? synthesize_enum(loc, *desc)
                           : builtin_id == syntax::builtin_t::STRUCT ? synthesize_struct(loc, *desc)
                                                                     : synthesize_union(loc, *desc);
        if (!synthesized) { return stdx::err<diagnostic>{std::move(synthesized).error()}; }

        auto meta{ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, **synthesized}]};
        meta->template resolve_if<types::meta_type>(**synthesized);
        return_type = meta;
        break;
    }
    default: UNREACHABLE("Builtin is not a type construction builtin");
    }
    return {};
}

// The target, build mode, and per-scope compiler settings
template <ast::IndexableID ID>
auto type_resolver::resolve_target_builtin(ID id, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::RETURN_ADDRESS:
        if (open_function_nodes_.empty()) {
            return make_sema_err("@returnAddress() may only be used inside of a function",
                                 error::ILLEGAL_RETURN_ADDRESS_USAGE,
                                 resolving_.ast.location_of(id));
        }
        return_type = &builtin.return_type;
        break;
    case syntax::builtin_t::TARGET_TRIPLE: {
        ASSERT(builtin.return_type.get_kind() == type_kind::SLICE);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::TARGET_OS:     return_type = &ctx_.get_builtin_type("Os"); break;
    case syntax::builtin_t::TARGET_ARCH:   return_type = &ctx_.get_builtin_type("Arch"); break;
    case syntax::builtin_t::TARGET_ABI:    return_type = &ctx_.get_builtin_type("Abi"); break;
    case syntax::builtin_t::TARGET_FAMILY: return_type = &ctx_.get_builtin_type("Family"); break;
    case syntax::builtin_t::OPTIMIZE_MODE:
        return_type = &ctx_.get_builtin_type("OptimizeMode");
        break;
    case syntax::builtin_t::RUNTIME_SAFETY:
        ASSERT(builtin.return_type.get_kind() == type_kind::BOOL);
        resolving_.scoped_runtime_safety.insert_or_assign(id.get_index(), scoped_runtime_safety());
        return_type = &builtin.return_type;
        break;
    case syntax::builtin_t::BRANCH_HINT: {
        const auto      arg{*call.arguments[0].as_opt<ast::expr_handle>()};
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      cv{evaluator.try_eval(arg)};
        const auto      variant{cv ? cv->as_opt<gir::const_enum>() : stdx::none};
        if (!variant) {
            return make_sema_err("@branchHint requires a compile-time 'builtin.BranchHint'",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(arg));
        }

        const auto placement{active_blocks_.empty() || active_blocks_.back().current_stmt_idx != 0
                                 ? stdx::none
                                 : hint_site_of(*active_blocks_.back().block)};
        if (!placement) {
            return make_sema_err("@branchHint must be the first statement of an 'if'/'else' "
                                 "branch, a 'match' arm, or a function body",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        const auto hint{ast::branch_hint_from_name(variant->name)};
        const bool fits_fn_body{hint == ast::branch_hint::COLD || hint == ast::branch_hint::NONE};
        if (*placement == hint_site::FN_BODY && !fits_fn_body) {
            return make_sema_err("A function body only accepts '@branchHint(.cold)'",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(arg));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    case syntax::builtin_t::SET_RUNTIME_SAFETY: {
        if (active_blocks_.empty() || return_trackers_.empty()) {
            return make_sema_err("@setRuntimeSafety can only be used within a function body",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        const auto      arg{*call.arguments[0].as_opt<ast::expr_handle>()};
        gir::const_eval evaluator{ctx_, resolving_};
        const auto      cv{evaluator.try_eval(arg)};
        const auto      enabled{cv ? cv->as_opt<bool>() : stdx::none};
        if (!enabled) {
            return make_sema_err("@setRuntimeSafety requires a compile-time 'bool'",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(arg));
        }
        active_blocks_.back().runtime_safety  = *enabled;
        active_blocks_.back().safety_fn_depth = function_boundaries_.size();
        return_type                           = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    case syntax::builtin_t::TARGET_ENDIAN:   return_type = &ctx_.get_builtin_type("Endian"); break;
    case syntax::builtin_t::TARGET_PTR_BITS: {
        ASSERT(builtin.return_type.get_kind() == type_kind::COMPTIME_INT);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::SET_EVAL_RECURSION_LIMIT: {
        if (return_trackers_.empty()) {
            return make_sema_err("@setEvalRecursionLimit can only be used within a function scope",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    case syntax::builtin_t::SET_EVAL_UNROLL_LIMIT: {
        if (return_trackers_.empty()) {
            return make_sema_err("@setEvalUnrollLimit can only be used within a function scope",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    case syntax::builtin_t::EXPORT: {
        if (auto problem{record_export(call)}) { return stdx::err{std::move(*problem)}; }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
        break;
    }
    default: UNREACHABLE("Builtin is not a target builtin");
    }
    return {};
}

// Integer bit counts, float math, and the checked and wrapping arithmetic
template <ast::IndexableID ID>
auto type_resolver::resolve_math_builtin(ID, builtin_site& site) -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::CLZ:
    case syntax::builtin_t::CTZ:
    case syntax::builtin_t::POPCOUNT: {
        auto&      operand_type{comptime_numeric_view(*get_call_arg_value_type(call.arguments[0]))};
        const auto width{integer_effective_bits(operand_type, target_ptr_bits())};
        if (width == 0 && operand_type.get_kind() != type_kind::INT) {
            return make_sema_err(fmt::format("'{}' operand must be an integer type; found '{}'",
                                             syntax::builtin_name(builtin_id),
                                             ctx_.type_display_name(operand_type)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[0]));
        }
        const auto result_bits{std::bit_width(static_cast<u64>(width))};
        return_type = &ctx_.get_int(static_cast<u16>(result_bits), false);
        break;
    }
    // @TypeOf returns a type as per documentation, but it's not the literal `type` type
    case syntax::builtin_t::SQRT:
    case syntax::builtin_t::SIN:
    case syntax::builtin_t::COS:
    case syntax::builtin_t::TAN:
    case syntax::builtin_t::EXP:
    case syntax::builtin_t::EXP2:
    case syntax::builtin_t::LOG:
    case syntax::builtin_t::LOG2:
    case syntax::builtin_t::LOG10:
    case syntax::builtin_t::FLOOR:
    case syntax::builtin_t::CEIL:  {
        const auto& arg{call.arguments[0]};
        auto&       operand{*get_call_arg_value_type(arg)};
        if (operand.is_poison()) { break; }
        const auto name{syntax::builtin_name(builtin_id)};
        if (call_arg_denotes_type(arg)) {
            return make_sema_err(
                fmt::format("'{}' expects a float value, but was given a type", name),
                error::TYPE_USED_AS_VALUE,
                get_call_arg_location(arg));
        }
        const auto kind{operand.get_kind()};
        if (is_integer(kind)) {
            return make_sema_err(
                fmt::format("'{}' operand must be a float; found '{}'; convert it with "
                            "`@floatFromInt`",
                            name,
                            ctx_.type_display_name(operand)),
                error::TYPE_MISMATCH,
                get_call_arg_location(arg));
        }
        if (is_float(kind)) {
            return_type = ctx_.pool.with_const(operand, false);
            break;
        }
        if (!is_comptime_numeric(kind)) {
            return make_sema_err(fmt::format("'{}' operand must be a float; found '{}'",
                                             name,
                                             ctx_.type_display_name(operand)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(arg));
        }
        // An untyped constant takes the float type its context asks for, and is computed there
        const auto expected{implicit_type_stack_.peek()};
        if (expected && is_float(expected->get_kind())) {
            return_type = ctx_.pool.with_const(*expected, false);
        } else {
            return_type = &ctx_.get_builtin_resolved_type(type_kind::COMPTIME_FLOAT);
        }
        break;
    }
    // These return @TypeOf(expression) which is trivial
    case syntax::builtin_t::ABS: {
        return_type = get_call_arg_value_type(call.arguments[0]);
        break;
    }
    case syntax::builtin_t::MUL_ADD: {
        return_type = get_call_arg_value_type(call.arguments[0]);
        // Every operand is a `T`, the first argument
        for (usize i{1}; return_type && i < call.arguments.size(); ++i) {
            const auto operand_type{get_call_arg_value_type(call.arguments[i])};
            if (operand_type && !is_assignable(*operand_type, *return_type)) {
                return make_sema_err(fmt::format("'@mulAdd' operands must be '{}'; found '{}'",
                                                 ctx_.type_display_name(*return_type),
                                                 ctx_.type_display_name(*operand_type)),
                                     error::OPERATOR_TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[i]));
            }
        }
        break;
    }
    case syntax::builtin_t::MIN:
    case syntax::builtin_t::MAX:
    case syntax::builtin_t::DIV_TRUNC:
    case syntax::builtin_t::DIV_FLOOR:
    case syntax::builtin_t::DIV_EXACT:
    case syntax::builtin_t::REM:
    case syntax::builtin_t::MOD:       {
        auto&      lhs_type{*get_call_arg_value_type(call.arguments[0])};
        auto&      rhs_type{*get_call_arg_value_type(call.arguments[1])};
        const bool floats_ok{builtin_id == syntax::builtin_t::MIN ||
                             builtin_id == syntax::builtin_t::MAX};
        const auto accepts{
            [&](type_kind k) -> bool { return floats_ok ? is_numeric(k) : is_integer(k); }};
        // The operands meet at their peer type, which is also the result
        const std::array operands{peer_operand{&lhs_type}, peer_operand{&rhs_type}};
        const bool both_numeric{is_numeric(lhs_type.get_kind()) && is_numeric(rhs_type.get_kind())};
        const auto peer{peer_type(ctx_, operands)};
        if (both_numeric && !peer) {
            return make_sema_err(fmt::format("'{}': {}",
                                             syntax::builtin_name(builtin_id),
                                             peer_error_message(ctx_, operands, peer.error())),
                                 error::NO_PEER_TYPE,
                                 get_call_arg_location(call.arguments[0]));
        }
        auto& operand_type{comptime_numeric_view(peer ? **peer : lhs_type)};
        if (!both_numeric || !accepts(operand_type.get_kind())) {
            return make_sema_err(
                fmt::format("'{}' expects two {} operands; found '{}' and '{}'",
                            syntax::builtin_name(builtin_id),
                            floats_ok ? "numeric" : "integer",
                            ctx_.type_display_name(comptime_numeric_view(lhs_type)),
                            ctx_.type_display_name(comptime_numeric_view(rhs_type))),
                error::OPERATOR_TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        // Two untyped operands stay untyped, so `@max(0, 1 << 100)` is a `comptime_int`
        auto& result_type{peer && is_comptime_numeric((*peer)->get_kind()) ? **peer : operand_type};
        return_type = ctx_.pool.with_const(result_type, false);
        break;
    }
    case syntax::builtin_t::ADD_WITH_OVERFLOW:
    case syntax::builtin_t::SUB_WITH_OVERFLOW:
    case syntax::builtin_t::MUL_WITH_OVERFLOW:
    case syntax::builtin_t::SHL_WITH_OVERFLOW: {
        auto& lhs_type{*get_call_arg_value_type(call.arguments[0])};
        auto& rhs_type{*get_call_arg_value_type(call.arguments[1])};
        auto& out_type{*get_resolved_call_arg_type(call.arguments[2])};
        // A shift's count is not a peer of the value shifted. The others meet at their peer type
        const bool       is_shift{builtin_id == syntax::builtin_t::SHL_WITH_OVERFLOW};
        const std::array operands{peer_operand{&lhs_type}, peer_operand{&rhs_type}};
        const auto       peer{is_shift ? stdx::result<gsl::not_null<type*>, peer_error>{&lhs_type}
                                       : peer_type(ctx_, operands)};
        const auto       is_int_operand{[](const type& t) {
            return is_integer(t.get_kind()) || is_comptime_int(t.get_kind());
        }};
        const bool       integers{is_int_operand(lhs_type) && is_int_operand(rhs_type)};
        if (integers && !peer) {
            return make_sema_err(fmt::format("'{}': {}",
                                             syntax::builtin_name(builtin_id),
                                             peer_error_message(ctx_, operands, peer.error())),
                                 error::NO_PEER_TYPE,
                                 get_call_arg_location(call.arguments[0]));
        }
        // A shift's count may be any integer type
        if (!integers) {
            return make_sema_err(
                fmt::format("'{}' expects two integer operands; found '{}' and '{}'",
                            syntax::builtin_name(builtin_id),
                            ctx_.type_display_name(lhs_type),
                            ctx_.type_display_name(rhs_type)),
                error::OPERATOR_TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        // The result slot is a mutable reference (`&mut T`). A `^mut T` pointer is also accepted.
        const auto  out_ref{out_type.get_data().as_opt<types::reference>()};
        const auto  out_ptr{out_type.get_data().as_opt<types::pointer>()};
        const type* out_underlying{out_ref   ? &out_ref->underlying
                                   : out_ptr ? &out_ptr->underlying
                                             : nullptr};
        // Untyped operands take the result slot's type
        const bool untyped{is_comptime_int((*peer)->get_kind())};
        const bool slot_matches{out_underlying &&
                                (untyped ? is_integer(out_underlying->get_kind())
                                         : is_same_unqualified(*out_underlying, **peer))};
        if (!slot_matches || out_type.is_constant()) {
            return make_sema_err(
                fmt::format("'{}' expects its third argument to be a '&mut {}' result reference; "
                            "found '{}'",
                            syntax::builtin_name(builtin_id),
                            ctx_.type_display_name(comptime_numeric_view(**peer)),
                            ctx_.type_display_name(out_type)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[2]));
        }
        return_type = &ctx_.get_builtin_resolved_type(type_kind::BOOL);
        break;
    }
    default: UNREACHABLE("Builtin is not a math builtin");
    }
    return {};
}

// `@memcpy`, `@memset`, and `@memmove`
template <ast::IndexableID ID>
auto type_resolver::resolve_memory_builtin(ID, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::MEMCPY:
    case syntax::builtin_t::MEMSET:
    case syntax::builtin_t::MEMMOVE: {
        ASSERT(builtin.return_type.get_kind() == type_kind::VOID_);
        const bool is_set{builtin_id == syntax::builtin_t::MEMSET};
        const auto op_name{syntax::builtin_name(builtin_id)};

        // Peel one `&`/`^` layer, then report {element type, is_writable} for a slice or array.
        const auto contiguous_of{[](type& t) -> stdx::option<std::pair<type&, bool>> {
            stdx::option<type&> u{t};
            if (const auto r{u->get_data().as_opt<types::reference>()}) {
                u.emplace(r->underlying);
            } else if (const auto p{u->get_data().as_opt<types::pointer>()}) {
                u.emplace(p->underlying);
            }
            if (const auto s{u->get_data().as_opt<types::slice>()}) {
                return std::pair<type&, bool>{s->underlying, !u->is_constant()};
            }
            if (const auto a{u->get_data().as_opt<types::array>()}) {
                return std::pair<type&, bool>{a->underlying, !u->is_constant()};
            }
            return stdx::none;
        }};

        auto&      dest_t{*get_resolved_call_arg_type(call.arguments[0])};
        const auto dest{contiguous_of(dest_t)};
        if (!dest) {
            return make_sema_err(
                fmt::format("'{}' expects a slice or array destination; found '{}'",
                            op_name,
                            ctx_.type_display_name(dest_t)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        if (!dest->second) {
            return make_sema_err(
                fmt::format("'{}' cannot write through an immutable destination; use a `mut` "
                            "slice or array",
                            op_name),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }

        if (is_set) {
            auto& val_t{*get_resolved_call_arg_type(call.arguments[1])};
            if (!is_integer(val_t.get_kind()) && val_t.get_kind() != type_kind::COMPTIME_INT) {
                return make_sema_err(
                    fmt::format("'@memset' fill value must be a byte-valued integer; found '{}'",
                                ctx_.type_display_name(val_t)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[1]));
            }
        } else {
            auto&      src_t{*get_resolved_call_arg_type(call.arguments[1])};
            const auto src{contiguous_of(src_t)};
            if (!src) {
                return make_sema_err(fmt::format("'{}' expects a slice or array source; found '{}'",
                                                 op_name,
                                                 ctx_.type_display_name(src_t)),
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[1]));
            }
            if (!is_same_unqualified(dest->first, src->first)) {
                return make_sema_err(
                    fmt::format("'{}' requires matching element types; the destination holds "
                                "'{}' but the source holds '{}'",
                                op_name,
                                ctx_.type_display_name(dest->first),
                                ctx_.type_display_name(src->first)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[1]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    default: UNREACHABLE("Builtin is not a memory builtin");
    }
    return {};
}

// Atomic loads, stores, read-modify-writes, exchanges, and fences
template <ast::IndexableID ID>
auto type_resolver::resolve_atomic_builtin(ID, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::ATOMIC_LOAD:
    case syntax::builtin_t::ATOMIC_STORE:
    case syntax::builtin_t::ATOMIC_RMW:
    case syntax::builtin_t::CMPXCHG_WEAK:
    case syntax::builtin_t::CMPXCHG_STRONG:
    case syntax::builtin_t::FENCE:          {
        const auto builtin_name{syntax::builtin_name(builtin_id)};

        if (builtin_id == syntax::builtin_t::FENCE) {
            const auto order{resolve_const_enum_arg(call.arguments[0], builtin_name, "order")};
            if (!order) { return stdx::err{order.error()}; }
            if (!is_valid_memory_order_name(order->name)) {
                return make_sema_err(fmt::format("'{}' cannot use memory order '{}'; a fence "
                                                 "accepts relaxed, acquire, release, acq_rel, "
                                                 "or seq_cst",
                                                 builtin_name,
                                                 order->name),
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[0]));
            }
            return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
            break;
        }

        // `@atomicStore` has no leading `T: type` argument; `T` is `ptr`'s pointee instead.
        const bool  has_t_arg{builtin_id != syntax::builtin_t::ATOMIC_STORE};
        const usize ptr_idx{has_t_arg ? 1UZ : 0UZ};

        auto&      ptr_type{*get_resolved_call_arg_type(call.arguments[ptr_idx])};
        const auto ptr_data{ptr_type.get_data().as_opt<types::pointer>()};
        if (!ptr_data) {
            return make_sema_err(fmt::format("'{}' expects 'ptr' to be a pointer ('^T' / '^mut "
                                             "T'); found '{}'",
                                             builtin_name,
                                             ctx_.type_display_name(ptr_type)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[ptr_idx]));
        }
        if (has_t_arg && !call_arg_denotes_type(call.arguments[0]) &&
            get_resolved_call_arg_type(call.arguments[0])->get_kind() != type_kind::TYPE) {
            return make_sema_err(
                fmt::format("'{}' expects a type as its first argument", builtin_name),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[0]));
        }
        auto& t{has_t_arg ? denoted_type(*get_resolved_call_arg_type(call.arguments[0]))
                          : ptr_data->underlying};

        const bool needs_mut_ptr{builtin_id != syntax::builtin_t::ATOMIC_LOAD};
        if (needs_mut_ptr && ptr_type.is_constant()) {
            return make_sema_err(
                fmt::format(
                    "'{}' expects 'ptr' to point to a mutable location ('^mut {}'); found '{}'",
                    builtin_name,
                    ctx_.type_display_name(t),
                    ctx_.type_display_name(ptr_type)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[ptr_idx]));
        }
        if (!is_same_unqualified(ptr_data->underlying, t)) {
            return make_sema_err(fmt::format("'{}' expects 'ptr' to point to '{}'; found '{}'",
                                             builtin_name,
                                             ctx_.type_display_name(t),
                                             ctx_.type_display_name(ptr_type)),
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[ptr_idx]));
        }

        const auto width{atomic_operand_width(t, target_ptr_bits())};
        const bool width_ok{width && (*width == 8 || *width == 16 || *width == 32 || *width == 64 ||
                                      (*width == 128 && target_has_128bit_atomics()))};
        if (!width_ok) {
            return make_sema_err(
                fmt::format("'{}' operand type '{}' has no native atomic width on this target; "
                            "8/16/32/64 bits are always supported, 128 only on x86_64/aarch64",
                            builtin_name,
                            ctx_.type_display_name(t)),
                error::TYPE_MISMATCH,
                get_call_arg_location(call.arguments[ptr_idx]));
        }

        // Every non-`ptr`/order/op operand (`val`, `expected`, `new`) must agree with `T`.
        const auto check_matches_t = [&](usize            arg_idx,
                                         std::string_view name) -> stdx::option<diagnostic> {
            auto& arg_type{*get_resolved_call_arg_type(call.arguments[arg_idx])};
            if (call_arg_denotes_type(call.arguments[arg_idx])) {
                return diagnostic{fmt::format("'{}' expects '{}' to be a value, but was given a "
                                              "type",
                                              builtin_name,
                                              name),
                                  error::TYPE_USED_AS_VALUE,
                                  get_call_arg_location(call.arguments[arg_idx])};
            }
            const bool ok{
                is_same_unqualified(arg_type, t) ||
                (arg_type.get_kind() == type_kind::COMPTIME_INT && is_integer(t.get_kind())) ||
                (arg_type.get_kind() == type_kind::COMPTIME_FLOAT && is_float(t.get_kind()))};
            if (ok) { return stdx::none; }
            return diagnostic{fmt::format("'{}' expects '{}' to be '{}'; found '{}'",
                                          builtin_name,
                                          name,
                                          ctx_.type_display_name(t),
                                          ctx_.type_display_name(arg_type)),
                              error::TYPE_MISMATCH,
                              get_call_arg_location(call.arguments[arg_idx])};
        };

        switch (builtin_id) {
        case syntax::builtin_t::ATOMIC_LOAD: {
            const auto order{resolve_const_enum_arg(call.arguments[2], builtin_name, "order")};
            if (!order) { return stdx::err{order.error()}; }
            if (order->name != "relaxed" && order->name != "acquire" && order->name != "seq_cst") {
                return make_sema_err(
                    fmt::format("'{}' cannot use memory order '{}'; loads accept relaxed, "
                                "acquire, or seq_cst",
                                builtin_name,
                                order->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[2]));
            }
            return_type = &t;
            break;
        }
        case syntax::builtin_t::ATOMIC_STORE: {
            if (auto diag{check_matches_t(1, "val")}) { return stdx::err{std::move(*diag)}; }
            const auto order{resolve_const_enum_arg(call.arguments[2], builtin_name, "order")};
            if (!order) { return stdx::err{order.error()}; }
            if (order->name != "relaxed" && order->name != "release" && order->name != "seq_cst") {
                return make_sema_err(
                    fmt::format("'{}' cannot use memory order '{}'; stores accept relaxed, "
                                "release, or seq_cst",
                                builtin_name,
                                order->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[2]));
            }
            return_type = &ctx_.get_builtin_resolved_type(type_kind::VOID_);
            break;
        }
        case syntax::builtin_t::ATOMIC_RMW: {
            const auto op{resolve_const_enum_arg(call.arguments[2], builtin_name, "op")};
            if (!op) { return stdx::err{op.error()}; }
            if (!is_valid_atomic_rmw_op_name(op->name)) {
                return make_sema_err(fmt::format("'{}' has no atomic RMW operation named '{}'",
                                                 builtin_name,
                                                 op->name),
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[2]));
            }
            if (auto diag{check_matches_t(3, "val")}) { return stdx::err{std::move(*diag)}; }
            const auto order{resolve_const_enum_arg(call.arguments[4], builtin_name, "order")};
            if (!order) { return stdx::err{order.error()}; }
            if (!is_valid_memory_order_name(order->name)) {
                return make_sema_err(
                    fmt::format("'{}' cannot use memory order '{}'", builtin_name, order->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[4]));
            }
            return_type = &t;
            break;
        }
        case syntax::builtin_t::CMPXCHG_WEAK:
        case syntax::builtin_t::CMPXCHG_STRONG: {
            if (auto diag{check_matches_t(2, "expected")}) { return stdx::err{std::move(*diag)}; }
            if (auto diag{check_matches_t(3, "new")}) { return stdx::err{std::move(*diag)}; }

            const auto succ{resolve_const_enum_arg(call.arguments[4], builtin_name, "succ_order")};
            if (!succ) { return stdx::err{succ.error()}; }
            if (!is_valid_memory_order_name(succ->name)) {
                return make_sema_err(fmt::format("'{}' cannot use success memory order '{}'",
                                                 builtin_name,
                                                 succ->name),
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[4]));
            }
            const auto fail{resolve_const_enum_arg(call.arguments[5], builtin_name, "fail_order")};
            if (!fail) { return stdx::err{fail.error()}; }
            if (fail->name == "release" || fail->name == "acq_rel") {
                return make_sema_err(
                    fmt::format("'{}' cannot use failure memory order '{}'; 'fail_order' may not "
                                "be release or acq_rel",
                                builtin_name,
                                fail->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[5]));
            }
            if (!is_valid_memory_order_name(fail->name) ||
                memory_order_rank(fail->name) > memory_order_rank(succ->name)) {
                return make_sema_err(
                    fmt::format("'{}' failure memory order '{}' may not be stronger than its "
                                "success order '{}'",
                                builtin_name,
                                fail->name,
                                succ->name),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[5]));
            }

            // `out` follows the `_withOverflow` convention: `&mut T` or `^mut T`.
            auto&       out_type{*get_resolved_call_arg_type(call.arguments[6])};
            const auto  out_ref{out_type.get_data().as_opt<types::reference>()};
            const auto  out_ptr{out_type.get_data().as_opt<types::pointer>()};
            const type* out_underlying{out_ref   ? &out_ref->underlying
                                       : out_ptr ? &out_ptr->underlying
                                                 : nullptr};
            if (!out_underlying || out_type.is_constant() ||
                !is_same_unqualified(*out_underlying, t)) {
                return make_sema_err(
                    fmt::format("'{}' expects its 'out' argument to be a '&mut {}' result "
                                "reference; found '{}'",
                                builtin_name,
                                ctx_.type_display_name(t),
                                ctx_.type_display_name(out_type)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[6]));
            }
            return_type = &ctx_.get_builtin_resolved_type(type_kind::BOOL);
            break;
        }
        default: UNREACHABLE("Unhandled atomic builtin");
        }
        break;
    }
    default: UNREACHABLE("Builtin is not a atomic builtin");
    }
    return {};
}

// C variadic argument access
template <ast::IndexableID ID>
auto type_resolver::resolve_c_variadic_builtin(ID, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::C_VA_START:
    case syntax::builtin_t::C_VA_COPY:
    case syntax::builtin_t::C_VA_END:
    case syntax::builtin_t::C_VA_ARG:   {
        // Every va builtin addresses caller-provided `va_list` storage through a pointer
        const auto list_arg_count{builtin_id == syntax::builtin_t::C_VA_COPY ? 2UZ : 1UZ};
        const auto builtin_name{syntax::builtin_name(builtin_id)};
        for (usize i{0}; i < list_arg_count; ++i) {
            const auto& list_type{*get_resolved_call_arg_type(call.arguments[i])};
            if (list_type.get_kind() != type_kind::POINTER || list_type.is_constant()) {
                return make_sema_err(
                    fmt::format("'{}' expects a `^mut` pointer to `va_list` storage; found '{}'",
                                builtin_name,
                                ctx_.type_display_name(list_type)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[i]));
            }
        }
        if (builtin_id == syntax::builtin_t::C_VA_ARG) {
            auto&      read_type{denoted_type(*get_resolved_call_arg_type(call.arguments[1]))};
            const auto read_kind{read_type.get_kind()};
            const bool generic_placeholder{read_kind == type_kind::TYPE &&
                                           !call_arg_denotes_type(call.arguments[1])};
            const bool readable{call_arg_denotes_type(call.arguments[1]) &&
                                (is_numeric(read_kind) || read_kind == type_kind::BOOL ||
                                 read_kind == type_kind::POINTER || read_kind == type_kind::ENUM ||
                                 read_kind == type_kind::STRUCT || read_kind == type_kind::UNION ||
                                 read_kind == type_kind::ARRAY || read_kind == type_kind::SLICE)};
            if (!read_type.is_poison() && !generic_placeholder && !readable) {
                return make_sema_err(
                    fmt::format("'{}' expects the type of the argument to read; found '{}'",
                                builtin_name,
                                ctx_.type_display_name(read_type)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[1]));
            }
            return_type = &read_type;
        } else {
            ASSERT(builtin.return_type.get_kind() == type_kind::VOID_);
            return_type = &builtin.return_type;
        }
        break;
    }
    default: UNREACHABLE("Builtin is not a c variadic builtin");
    }
    return {};
}

// Panics, tests, assertions, compile-time errors, entry points, and intrinsics
template <ast::IndexableID ID>
auto type_resolver::resolve_program_builtin(ID, builtin_site& site)
    -> stdx::result<void, diagnostic> {
    const auto& call{site.call};
    const auto& builtin{site.builtin};
    const auto  builtin_id{site.kind};
    auto&       return_type{site.return_type};
    using syntax::builtin_t;
    using syntax::token_type_t;
    switch (site.kind) {
    case syntax::builtin_t::PANIC: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        // The message must be compile-time known so it can be validated / interned
        if (!call.arguments.empty()) {
            bool is_const_string{false};
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)) {
                    is_const_string = true;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        is_const_string = val->is<std::string>();
                    }
                }
            }
            if (!is_const_string) {
                return make_sema_err("@panic message must be a compile-time-constant string",
                                     error::COMPTIME_EVALUATION_FAILED,
                                     get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::TRAP: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::SKIP: {
        ASSERT(builtin.return_type.get_kind() == type_kind::NORETURN);
        // An optional message must be compile-time known so it can be interned
        if (!call.arguments.empty()) {
            bool is_const_string{false};
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)) {
                    is_const_string = true;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        is_const_string = val->is<std::string>();
                    }
                }
            }
            if (!is_const_string) {
                return make_sema_err("@skip message must be a compile-time-constant string",
                                     error::COMPTIME_EVALUATION_FAILED,
                                     get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::COMPILE_ERROR: {
        if (in_comptime_loop_) {
            return_type = &builtin.return_type;
            break;
        }
        std::string message{"compilation aborted by @compileError"};
        if (!call.arguments.empty()) {
            if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
                if (const auto str_expr{resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)}) {
                    message = str_expr->value;
                } else {
                    gir::const_eval evaluator{ctx_, resolving_};
                    if (const auto val{evaluator.try_eval(*expr_h)}) {
                        if (auto str{val->as_string_opt()}) { message = std::move(*str); }
                    }
                }
            }
        }
        ctx_.diags.emplace_back(std::move(message),
                                error::COMPILE_ERROR_REACHED,
                                resolving_.ast.location_of(call.function));
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::SRC: {
        return_type = &ctx_.get_builtin_type("SourceLocation");
        break;
    }
    case syntax::builtin_t::EMBED: {
        if (call.arguments.empty()) {
            return make_sema_err("@embed expects 1 argument",
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }

        stdx::option<std::string> path_str;
        if (const auto expr_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
            if (const auto str_expr{resolving_.ast.get_as_opt<ast::string_expr>(*expr_h)}) {
                path_str.emplace(str_expr->value);
            } else {
                gir::const_eval evaluator{ctx_, resolving_};
                if (const auto val{evaluator.try_eval(*expr_h)}) {
                    if (const auto str{val->as_opt<std::string>()}) { path_str.emplace(*str); }
                }
            }
        }

        if (!path_str) {
            return make_sema_err("@embed argument must be a compile-time string",
                                 error::COMPTIME_EVALUATION_FAILED,
                                 get_call_arg_location(call.arguments[0]));
        }

        const auto embed_path{resolving_.make_path_absolute(*path_str)};
        const auto content_opt{ctx_.read_embed_file(embed_path)};
        if (!content_opt) {
            return make_sema_err(
                fmt::format("failed to read embedded file '{}'", embed_path.string()),
                error::COMPTIME_EVALUATION_FAILED,
                get_call_arg_location(call.arguments[0]));
        }

        return_type = &ctx_.get_array(
            types::mut::CONSTANT, true, content_opt->size(), ctx_.get_int(8, false));
        break;
    }
    case syntax::builtin_t::ENTRY_MAIN:
    case syntax::builtin_t::ENTRY_TEST_RUNNER:
    case syntax::builtin_t::TEST_FUNCTIONS:    {
        const auto name{syntax::builtin_name(builtin_id)};
        if (!resolving_.is_start) {
            return make_sema_err(fmt::format("'{}()' is only valid in the program's entry code "
                                             "(`std/start.gh`)",
                                             name),
                                 error::TYPE_MISMATCH,
                                 resolving_.ast.location_of(call.function));
        }
        if (builtin_id == syntax::builtin_t::TEST_FUNCTIONS) {
            auto& test{ctx_.get_builtin_type("Test")};
            return_type = &ctx_.get_slice(types::mut::CONSTANT, false, denoted_type(test));
            break;
        }
        if (builtin_id == syntax::builtin_t::ENTRY_TEST_RUNNER) {
            const auto runner_type{entry_test_runner_type()};
            if (!runner_type) {
                return make_sema_err(fmt::format("'{}()': the program has no 'test_runner'", name),
                                     error::TYPE_MISMATCH,
                                     resolving_.ast.location_of(call.function));
            }
            return_type = runner_type.get();
            break;
        }
        if (!ctx_.entry_root) {
            return_type = &ctx_.get_function({},
                                             ctx_.get_builtin_resolved_type(type_kind::VOID_),
                                             false,
                                             ast::calling_convention::C,
                                             false);
            break;
        }
        const auto main_type{entry_main_type()};
        if (!main_type) {
            return make_sema_err(
                fmt::format("'{}()': the program has no '{}' function", name, ctx_.user_main_name),
                error::TYPE_MISMATCH,
                resolving_.ast.location_of(call.function));
        }
        return_type = main_type.get();
        break;
    }
    case syntax::builtin_t::ENUM_LITERAL: {
        auto& literal{ctx_.get_builtin_resolved_type(type_kind::ENUM_LITERAL)};
        auto  meta{ctx_.pool[{type_kind::TYPE, types::mut::CONSTANT, literal}]};
        meta->resolve_if<types::meta_type>(literal);
        return_type = meta;
        break;
    }
    case syntax::builtin_t::LLVM_INTRINSIC: {
        stdx::option<std::string> name;
        if (const auto name_h{call.arguments[0].as_opt<ast::expr_handle>()}) {
            gir::const_eval evaluator{ctx_, resolving_};
            if (const auto val{evaluator.try_eval(*name_h)}) { name = val->as_string_opt(); }
        }
        if (!name) {
            return make_sema_err("'@llvmIntrinsic' needs a compile-time string naming the "
                                 "intrinsic, like \"llvm.x86.sse2.pause\"",
                                 error::COMPTIME_EVALUATION_FAILED,
                                 get_call_arg_location(call.arguments[0]));
        }
        if (!call_arg_denotes_type(call.arguments[1])) {
            return make_sema_err("'@llvmIntrinsic' expects the return type as its second argument",
                                 error::TYPE_MISMATCH,
                                 get_call_arg_location(call.arguments[1]));
        }
        auto& result{denoted_type(*get_resolved_call_arg_type(call.arguments[1]))};
        if (result.is_poison()) { break; }

        std::vector<const type*> arg_types;
        bool                     poisoned{false};
        for (usize i{2}; i < call.arguments.size(); ++i) {
            const auto& arg{call.arguments[i]};
            if (call_arg_denotes_type(arg)) {
                return make_sema_err("'@llvmIntrinsic' arguments are values, not types",
                                     error::TYPE_USED_AS_VALUE,
                                     get_call_arg_location(arg));
            }
            auto& arg_type{*get_resolved_call_arg_type(arg)};
            if (arg_type.is_poison()) {
                poisoned = true;
                break;
            }
            // An untyped literal is an `i32` / `f64`, as when it binds an `auto` parameter
            arg_types.emplace_back(&ctx_.default_concrete(arg_type));
        }
        if (poisoned) { break; }
        std::vector<usize> immediate_args;
        if (const auto problem{codegen::check_llvm_intrinsic(
                *name,
                result,
                arg_types,
                codegen::resolve_target_triple(ctx_.target_opts.triple_str),
                &immediate_args)}) {
            return make_sema_err(
                *problem, error::INVALID_LLVM_INTRINSIC, resolving_.ast.location_of(call.function));
        }
        // LLVM takes these as part of the instruction, so they must be known now
        for (const auto index : immediate_args) {
            const auto&     arg{call.arguments[index + 2]};
            gir::const_eval evaluator{ctx_, resolving_};
            const auto      arg_h{arg.as_opt<ast::expr_handle>()};
            if (!arg_h || !evaluator.try_eval(*arg_h)) {
                return make_sema_err(
                    fmt::format(
                        "argument {} of '{}' must be known at compile time", index + 1, *name),
                    error::COMPTIME_EVALUATION_FAILED,
                    get_call_arg_location(arg));
            }
        }
        return_type = &result;
        break;
    }
    case syntax::builtin_t::EXPECT:
    case syntax::builtin_t::REQUIRE: {
        if (const auto cond_expr{call.arguments[0].as_opt<ast::expr_handle>()}) {
            if (const auto ct{resolving_.get_sema_type_opt(*cond_expr)};
                ct && !ct->is_poison() && ct->get_kind() != type_kind::BOOL &&
                ct->get_kind() != type_kind::POINTER) {
                return make_sema_err(
                    fmt::format("{} condition must be `bool` or a pointer, found `{}` (wrap it in "
                                "`@boolFromInt(...)` if that is intended)",
                                syntax::builtin_name(builtin_id),
                                ctx_.type_display_name(*ct)),
                    error::TYPE_MISMATCH,
                    get_call_arg_location(call.arguments[0]));
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    case syntax::builtin_t::ASSERT:
    case syntax::builtin_t::VERIFY: {
        const auto* name{builtin_id == syntax::builtin_t::VERIFY ? "@verify" : "@assert"};
        const auto  cond_expr{call.arguments[0].as_opt<ast::expr_handle>()};

        gir::const_eval evaluator{ctx_, resolving_};

        // An optional message must be a compile-time-constant string
        std::string msg_text;
        if (call.arguments.size() > 1) {
            bool ok{false};
            if (const auto mh{call.arguments[1].as_opt<ast::expr_handle>()}) {
                if (const auto se{resolving_.ast.get_as_opt<ast::string_expr>(*mh)}) {
                    msg_text = se->value;
                    ok       = true;
                } else if (const auto mv{evaluator.try_eval(*mh)}) {
                    if (const auto s{mv->as_opt<std::string>()}) {
                        msg_text = *s;
                        ok       = true;
                    }
                }
            }
            if (!ok) {
                return make_sema_err(
                    fmt::format("{} message must be a compile-time-constant string", name),
                    error::COMPTIME_EVALUATION_FAILED,
                    get_call_arg_location(call.arguments[1]));
            }
        }

        if (cond_expr) {
            if (const auto ct{resolving_.get_sema_type_opt(*cond_expr)};
                ct && !ct->is_poison() && ct->get_kind() != type_kind::BOOL &&
                ct->get_kind() != type_kind::POINTER) {
                return make_sema_err(fmt::format("{} condition must be `bool` or a pointer, found "
                                                 "`{}` (wrap it in `@boolFromInt("
                                                 "...)` if that is intended)",
                                                 name,
                                                 ctx_.type_display_name(*ct)),
                                     error::TYPE_MISMATCH,
                                     get_call_arg_location(call.arguments[0]));
            }

            // A comptime-known-false condition is a compile error at the call site.
            if (const auto cv{evaluator.try_eval(*cond_expr)}) {
                bool is_false{false};
                if (const auto b{cv->as_opt<bool>()}) {
                    is_false = !*b;
                } else if (cv->is<gir::nullptr_val>()) {
                    is_false = true;
                }
                // An arm a folded condition rules out never runs
                if (is_false && !in_dead_arm()) {
                    ctx_.diags.emplace_back(
                        fmt::format("{} failed at compile time{}",
                                    name,
                                    msg_text.empty() ? "" : fmt::format(": {}", msg_text)),
                        error::STATIC_ASSERTION_FAILED,
                        resolving_.ast.location_of(call.function));
                }
            }
        }
        return_type = &builtin.return_type;
        break;
    }
    default: UNREACHABLE("Builtin is not a program builtin");
    }
    return {};
}

template <ast::IndexableID ID>
auto type_resolver::resolve_builtin_call(ID                             id,
                                         const ast::call_expr&          call,
                                         const types::builtin_function& builtin)
    -> stdx::result<void, diagnostic> {
    ASSERT(call.function.is<ast::identifier_expr>(), "Builtin function must be a raw ident");
    // There must be an actual builtin present with a token id
    const auto builtin_opt{call.function->get_builtin()};
    ASSERT(builtin_opt, "Cannot resolve non-builtin function");
    TRY(check_builtin_arity(call, builtin, *builtin_opt));

    builtin_site site{
        .call = call, .builtin = builtin, .kind = *builtin_opt, .return_type = &ctx_.get_poison()};
    switch (family_of(site.kind)) {
    case builtin_family::CAST:              TRY(resolve_cast_builtin(id, site)); break;
    case builtin_family::REFLECTION:        TRY(resolve_reflection_builtin(id, site)); break;
    case builtin_family::TYPE_CONSTRUCTION: TRY(resolve_type_construction_builtin(id, site)); break;
    case builtin_family::TARGET:            TRY(resolve_target_builtin(id, site)); break;
    case builtin_family::MATH:              TRY(resolve_math_builtin(id, site)); break;
    case builtin_family::MEMORY:            TRY(resolve_memory_builtin(id, site)); break;
    case builtin_family::ATOMIC:            TRY(resolve_atomic_builtin(id, site)); break;
    case builtin_family::C_VARIADIC:        TRY(resolve_c_variadic_builtin(id, site)); break;
    case builtin_family::PROGRAM:           TRY(resolve_program_builtin(id, site)); break;
    }

    resolving_.set_sema_type(id, *site.return_type);
    last_type_.emplace(*site.return_type);
    return {};
}

template auto type_resolver::resolve_builtin_call<ast::node_id>(ast::node_id,
                                                                const ast::call_expr&,
                                                                const types::builtin_function&)
    -> stdx::result<void, diagnostic>;

template auto type_resolver::resolve_builtin_call<ast::explicit_type_id>(
    ast::explicit_type_id, const ast::call_expr&, const types::builtin_function&)
    -> stdx::result<void, diagnostic>;

auto type_resolver::resolve_call_args(gsl::span<const ast::call_expr::argument> args)
    -> resolve_result {
    bool any_poison{false};
    for (const auto& arg : args) {
        any_poison |= arg.visit([this](auto id) -> bool {
            resolve(id);
            return last_type_.take()->is_poison();
        });
    }
    return any_poison ? resolve_result::POISONED : resolve_result::OK;
}

auto type_resolver::resolve_atomic_call_args(const ast::call_expr& call) -> resolve_result {
    const auto tok{call.function->get_builtin()};
    const auto roles{atomic_arg_roles(tok)};
    const auto args{gsl::span<const ast::call_expr::argument>{call.arguments}};
    if (args.size() != roles.size()) { return resolve_call_args(args); }

    // `T` is the leading type argument, or `ptr`'s pointee for `@atomicStore`
    const bool          has_t_arg{tok != syntax::builtin_t::ATOMIC_STORE};
    stdx::option<type&> operand_type;
    const auto          find_operand_type = [&] {
        if (has_t_arg && call_arg_denotes_type(args[0])) {
            operand_type.emplace(denoted_type(*get_resolved_call_arg_type(args[0])));
        } else if (const auto ptr{get_resolved_call_arg_type(args[has_t_arg ? 1 : 0])
                                      ->get_data()
                                      .as_opt<types::pointer>()}) {
            operand_type.emplace(*ctx_.pool.strip_volatile(ptr->underlying));
        }
    };

    bool any_poison{false};
    bool found_operand_type{false};
    for (usize i{0}; i < args.size(); ++i) {
        type* hint{nullptr};
        switch (roles[i]) {
        case atomic_arg::PLAIN: break;
        case atomic_arg::OPERAND:
            if (!found_operand_type && !any_poison) { find_operand_type(); }
            found_operand_type = true;
            if (operand_type) { hint = operand_type.get(); }
            break;
        case atomic_arg::ORDER: hint = &ctx_.get_builtin_type("MemoryOrder"); break;
        case atomic_arg::OP:    hint = &ctx_.get_builtin_type("AtomicRmwOp"); break;
        }
        const structural_guard g{implicit_type_stack_, hint};
        any_poison |= resolve_call_args(args.subspan(i, 1)) == resolve_result::POISONED;
    }
    return any_poison ? resolve_result::POISONED : resolve_result::OK;
}

auto type_resolver::comptime_numeric_view(type& t) -> type& { return ctx_.default_concrete(t); }

auto type_resolver::get_resolved_call_arg_type(const ast::call_expr::argument& arg)
    -> gsl::not_null<type*> {
    return &arg.visit(
        [this](ast::expr_handle id) -> auto& {
            // Labels store their actual type in nested node data
            if (const auto label{resolving_.ast.get_as_opt<ast::label_expr>(id)}) {
                if (label->name) {
                    auto type{resolving_.get_sema_type_opt(*label->name)};
                    ASSERT(type, "Labeled call arg was not typed");
                    return *type;
                }
            }

            auto type{resolving_.get_sema_type_opt(id)};
            ASSERT(type, "Call argument was not typed");
            return *type;
        },
        [this](ast::explicit_type_id id) -> auto& {
            auto type{resolving_.get_sema_type_opt(id)};
            ASSERT(type, "Call argument was not typed");
            return *type;
        });
}

auto type_resolver::get_call_arg_value_type(const ast::call_expr::argument& arg)
    -> gsl::not_null<type*> {
    const auto t{get_resolved_call_arg_type(arg)};
    if (const auto ref{t->get_data().as_opt<types::reference>()}) { return &ref->underlying; }
    return t;
}

auto type_resolver::get_call_arg_location(const ast::call_expr::argument& arg) -> source_location {
    return arg.visit([this](auto id) -> source_location { return resolving_.ast.location_of(id); });
}

auto type_resolver::expand_pack_call_args(const ast::call_expr& call)
    -> stdx::option<expanded_call_args> {
    expanded_call_args out;
    for (usize i{0}; i < call.arguments.size(); ++i) {
        const bool is_expansion{i < call.pack_expansions.size() && call.pack_expansions[i]};
        if (!is_expansion) {
            out.source_index.emplace_back(i);
            out.pack_k.emplace_back(stdx::none);
            continue;
        }
        const auto expr_h{call.arguments[i].as_opt<ast::expr_handle>()};
        const auto ident{expr_h ? resolving_.ast.get_as_opt<ast::identifier_expr>(*expr_h)
                                : stdx::none};
        if (!current_pack_ || !ident || ident->name != current_pack_->name) {
            ctx_.diags.emplace_back("'...' may only expand the enclosing parameter pack",
                                    error::PACK_EXPANSION_MISPLACED,
                                    get_call_arg_location(call.arguments[i]));
            return stdx::none;
        }
        for (usize k{0}; k < current_pack_->element_types.size(); ++k) {
            out.source_index.emplace_back(i);
            out.pack_k.emplace_back(k);
        }
    }
    return out;
}

auto type_resolver::resolve_field_name_string(const ast::call_expr::argument& name_arg)
    -> stdx::option<std::string> {
    const auto expr_h{name_arg.as_opt<ast::expr_handle>()};
    if (!expr_h) { return stdx::none; }
    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(*expr_h)};
    if (!folded) { return stdx::none; }
    const auto name{folded->as_opt<std::string>()};
    if (!name) { return stdx::none; }
    return std::string{*name};
}

auto type_resolver::resolve_field_by_name(const ast::call_expr::argument& name_arg, type& denoted)
    -> stdx::option<field_lookup_result> {
    const auto name{resolve_field_name_string(name_arg)};
    if (!name) { return stdx::none; }
    return find_aggregate_field(denoted, *name);
}

auto type_resolver::resolve_type_descriptor(const ast::call_expr::argument& desc_arg)
    -> stdx::option<gir::const_struct> {
    const auto expr_h{desc_arg.as_opt<ast::expr_handle>()};
    if (!expr_h) { return stdx::none; }
    gir::const_eval evaluator{ctx_, resolving_};
    const auto      folded{evaluator.try_eval(*expr_h)};
    if (!folded) { return stdx::none; }
    const auto s{folded->as_opt<gir::const_struct>()};
    if (!s) { return stdx::none; }
    return *s;
}

auto type_resolver::resolve_const_enum_arg(const ast::call_expr::argument& arg,
                                           std::string_view                builtin_name,
                                           std::string_view                what)
    -> stdx::result<gir::const_enum, diagnostic> {
    const auto loc{get_call_arg_location(arg)};
    const auto expr_h{arg.as_opt<ast::expr_handle>()};
    if (!expr_h) {
        return make_sema_err(
            fmt::format("'{}' expects '{}' to be a compile-time constant", builtin_name, what),
            error::TYPE_MISMATCH,
            loc);
    }
    gir::const_eval ce{ctx_, resolving_};
    const auto      val{ce.try_eval(*expr_h)};
    const auto      en{val ? val->as_opt<gir::const_enum>() : stdx::none};
    if (!en) {
        return make_sema_err(
            fmt::format("'{}' expects '{}' to be a compile-time constant", builtin_name, what),
            error::TYPE_MISMATCH,
            loc);
    }
    return *en;
}

// A concrete runtime type whose byte size is known without any further folding
auto type_resolver::bit_cast_width(const type& t) const -> stdx::option<u64> {
    if (const auto st{t.get_data().as_opt<types::struct_t>()}) {
        if (!st->is_bit_packed()) { return stdx::none; }
        return packed_backing_bits(*st, target_ptr_bits()).transform([](u32 b) { return u64{b}; });
    }
    if (const auto un{t.get_data().as_opt<types::union_t>()}) {
        if (!un->is_bit_packed()) { return stdx::none; }
        return packed_union_backing_bits(*un, target_ptr_bits()).transform([](u32 b) {
            return u64{b};
        });
    }
    if (!has_fixed_bit_layout(t)) { return stdx::none; }
    if (t.get_kind() == type_kind::INT) { return u64{int_width(t)}; }
    return u64{gir::const_eval::type_size_of(t, target_layout()) * 8};
}

auto type_resolver::has_fixed_bit_layout(const type& t) noexcept -> bool {
    if (t.is_poison() || is_generic_type(t)) { return false; }
    switch (t.get_kind()) {
    case type_kind::INT:
    case type_kind::ISIZE:
    case type_kind::USIZE:
    case type_kind::BOOL:
    case type_kind::F16:
    case type_kind::F32:
    case type_kind::F64:
    case type_kind::F80:
    case type_kind::F128:
    case type_kind::POINTER: return true;
    default:                 return false;
    }
}

auto type_resolver::target_has_x86_fp80() const -> bool {
    const auto arch{codegen::target_facts::resolve(ctx_.target_opts.triple_str).arch};
    return arch == "x86_64" || arch == "x86";
}

auto type_resolver::target_ptr_bits() const -> u32 {
    return codegen::target_facts::resolve(ctx_.target_opts.triple_str).ptr_bits;
}

auto type_resolver::target_layout() const -> codegen::type_layout {
    return codegen::type_layout::resolve(ctx_.target_opts.triple_str);
}

auto type_resolver::target_has_128bit_atomics() const -> bool {
    const auto arch{codegen::target_facts::resolve(ctx_.target_opts.triple_str).arch};
    return arch == "x86_64" || arch == "aarch64";
}

auto type_resolver::target_supports_callconv(ast::calling_convention conv) const -> bool {
    if (conv == ast::calling_convention::C) { return true; }
    const auto arch{codegen::target_facts::resolve(ctx_.target_opts.triple_str).arch};
    switch (conv) {
    case ast::calling_convention::SYSV:
    case ast::calling_convention::WIN64:        return arch == "x86_64";
    case ast::calling_convention::X86_STDCALL:
    case ast::calling_convention::X86_FASTCALL: return arch == "x86" || arch == "x86_64";
    case ast::calling_convention::AAPCS:        return arch == "arm" || arch == "thumb";
    default:                                    return true;
    }
}

namespace {

// What the C ABI lowering would have to rewrite: a struct, union, array, slice, or erased callable
[[nodiscard]] auto passes_aggregate_by_value(const type& t) -> bool {
    if (is_generic_type(t)) { return false; }
    return t.get_data().visit(
        [](const types::struct_t& s) { return !s.is_bit_packed() && !s.fields.empty(); },
        [](const types::union_t& u) { return !u.is_bit_packed() && !u.fields.empty(); },
        [](const types::array& arr) { return arr.len > 0 || arr.null_terminated; },
        [](const types::slice&) { return true; },
        [](const types::closure_t&) { return true; },
        [](const types::function& fn) { return fn.erased; },
        [](const auto&) { return false; });
}

} // namespace

auto type_resolver::unmodeled_c_abi_error(const type&            fn_type,
                                          const source_location& location) const
    -> stdx::option<diagnostic> {
    const auto fn{fn_type.get_data().as_opt<types::function>()};
    if (!fn) { return stdx::none; }
    const auto triple{codegen::resolve_target_triple(ctx_.target_opts.triple_str)};
    if (codegen::has_c_abi_model(triple, codegen::to_llvm_callconv(fn->conv, triple))) {
        return stdx::none;
    }

    const type* offender{passes_aggregate_by_value(fn->return_type) ? &fn->return_type : nullptr};
    for (const auto* param : fn->params) {
        if (!offender && passes_aggregate_by_value(*param)) { offender = param; }
    }
    if (!offender) { return stdx::none; }
    return diagnostic{fmt::format("'{}' passes '{}' by value, but ghoti doesn't know how C passes "
                                  "aggregates on '{}' yet; pass a pointer to it instead",
                                  ctx_.type_display_name(fn_type),
                                  ctx_.type_display_name(*offender),
                                  triple.str()),
                      error::UNSUPPORTED_TARGET,
                      location};
}

} // namespace ghoti::sema
