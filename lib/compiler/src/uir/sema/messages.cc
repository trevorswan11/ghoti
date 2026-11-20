#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdx/types.hh>

#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"

namespace ghoti::uir::sema {

namespace messages {

namespace {

// " (reason)" when the conversion has a named reason, else nothing
[[nodiscard]] auto
reason_suffix(const gs::context& ctx, const gs::type& from, const gs::type& to, u32 ptr_bits)
    -> std::string {
    if (const auto reason{gs::cast_rejection_reason(from, to, ptr_bits, ctx.user_type_names)}) {
        return fmt::format(" ({})", *reason);
    }

    return {};
}

} // namespace

auto store_mismatch(const gs::context& ctx, const gs::type& from, const gs::type& to, u32 ptr_bits)
    -> std::string {
    return ctx.store_mismatch_message(from, to, ptr_bits);
}

auto return_mismatch(const gs::context& ctx, const gs::type& from, const gs::type& to, u32 ptr_bits)
    -> std::string {
    return fmt::format("Return value of type '{}' is not assignable to function return type '{}'{}",
                       ctx.type_display_name(from),
                       ctx.type_display_name(to),
                       reason_suffix(ctx, from, to, ptr_bits));
}

auto argument_mismatch(const gs::context& ctx,
                       usize              index,
                       const gs::type&    from,
                       const gs::type&    to,
                       std::string_view   callee,
                       u32                ptr_bits) -> std::string {
    return fmt::format(
        "Argument {} of type '{}' is not assignable to parameter type '{}' in call to '{}'{}",
        index,
        ctx.type_display_name(from),
        ctx.type_display_name(to),
        callee,
        reason_suffix(ctx, from, to, ptr_bits));
}

auto operator_mismatch(const gs::context& ctx,
                       std::string_view   op,
                       const gs::type&    lhs,
                       const gs::type&    rhs) -> std::string {
    return fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                       op,
                       ctx.type_display_name(lhs),
                       ctx.type_display_name(rhs));
}

auto literal_out_of_range(const gs::context& ctx, const gs::type& to) -> std::string {
    return fmt::format("integer literal is out of range for type '{}'", ctx.type_display_name(to));
}

auto value_out_of_range(const gs::context& ctx, const big_int& value, const gs::type& to)
    -> std::string {
    return fmt::format("integer value {} is out of range for type '{}'",
                       value.to_string(),
                       ctx.type_display_name(to));
}

auto condition_not_bool(const gs::context& ctx, const gs::type& found) -> std::string {
    return fmt::format("Conditional branch condition must be of type 'bool'; found '{}'",
                       ctx.type_display_name(found));
}

} // namespace messages

auto operator_spelling(tag t) noexcept -> std::string_view {
    switch (t) {
    case tag::add:      return "+";
    case tag::sub:      return "-";
    case tag::mul:      return "*";
    case tag::div:      return "/";
    case tag::mod:      return "%";
    case tag::add_wrap: return "+%";
    case tag::sub_wrap: return "-%";
    case tag::mul_wrap: return "*%";
    case tag::add_sat:  return "+|";
    case tag::sub_sat:  return "-|";
    case tag::mul_sat:  return "*|";
    case tag::shl_sat:  return "<<|";
    case tag::shl_wrap: return "<<%";
    case tag::shl:      return "<<";
    case tag::shr:      return ">>";
    case tag::bit_and:  return "&";
    case tag::bit_or:   return "|";
    case tag::bit_xor:  return "^";
    case tag::cmp_eq:   return "==";
    case tag::cmp_ne:   return "!=";
    case tag::cmp_lt:   return "<";
    case tag::cmp_le:   return "<=";
    case tag::cmp_gt:   return ">";
    case tag::cmp_ge:   return ">=";
    default:            return tag_name(t);
    }
}

} // namespace ghoti::uir::sema
