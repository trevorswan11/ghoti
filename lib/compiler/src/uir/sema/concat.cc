#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ip/pool.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

// An array's or slice's element type and whether it ends in a sentinel
struct sequence_shape {
    gs::type& elem;
    bool      null_terminated;
};

[[nodiscard]] auto sequence_of(gs::type& t) -> stdx::option<sequence_shape> {
    if (const auto arr{t.get_data().as_opt<gs::types::array>()}) {
        return sequence_shape{.elem = arr->underlying, .null_terminated = arr->null_terminated};
    }

    if (const auto sl{t.get_data().as_opt<gs::types::slice>()}) {
        return sequence_shape{.elem = sl->underlying, .null_terminated = sl->null_terminated};
    }

    return stdx::none;
}

} // namespace

auto analyzer::analyze_concat(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};
    auto&       ctx{comp_.ctx()};
    auto&       pool{comp_.constants()};
    const auto  lhs{TRY(resolve_value(d.bin.lhs, src))};
    const auto  rhs{TRY(resolve_value(d.bin.rhs, src))};
    auto&       lhs_type{type_of(lhs)};
    auto&       rhs_type{type_of(rhs)};
    const auto  lhs_shape{sequence_of(lhs_type)};
    const auto  rhs_shape{sequence_of(rhs_type)};
    if (!lhs_shape || !rhs_shape) {
        return comp_.report(
            u_,
            src,
            gs::error::CONCAT_REQUIRES_ARRAY_OR_SLICE,
            fmt::format("operator '++' requires two array or slice operands; found '{}' and '{}'",
                        ctx.type_display_name(lhs_type),
                        ctx.type_display_name(rhs_type)));
    }

    if (!gs::is_same_unqualified(lhs_shape->elem, rhs_shape->elem)) {
        return comp_.report(
            u_,
            src,
            gs::error::CONCAT_ELEM_TYPE_MISMATCH,
            fmt::format("operator '++' requires matching element types; found '{}' and '{}'",
                        ctx.type_display_name(lhs_shape->elem),
                        ctx.type_display_name(rhs_shape->elem)));
    }

    // A module-level aggregate `const` is known though runtime code reads its global
    const auto known_of{[&](const operand& op) -> stdx::option<ip::index> {
        if (const auto g{op.as_opt<global_const>()}) { return g->value; }
        return comptime_value(op);
    }};
    const auto lhs_known{known_of(lhs)};
    const auto rhs_known{known_of(rhs)};
    if (!lhs_known || !rhs_known) {
        const auto unknown{as_inst(!lhs_known ? d.bin.lhs : d.bin.rhs)};
        return comp_.report(u_,
                            unknown ? src_of(*unknown) : src,
                            gs::error::CONCAT_NOT_FOLDABLE,
                            "operands of '++' must be known at compile time");
    }

    // Reading both before interning, which may move what the pool holds
    const auto lhs_text{pool.value_at(*lhs_known).data.as_opt<std::string>()};
    const auto rhs_text{pool.value_at(*rhs_known).data.as_opt<std::string>()};
    // The old evaluator joins strings into an array of bytes, not a string
    const auto items_of{[&](ip::index known, const stdx::option<std::string>& text)
                            -> stdx::option<std::vector<stdx::option<ip::index>>> {
        if (text) {
            std::vector<stdx::option<ip::index>> bytes;
            for (const char c : *text) {
                bytes.emplace_back(pool.intern(u64{static_cast<u8>(c)}, lhs_shape->elem));
            }
            return bytes;
        }

        const auto items{pool.value_at(known).data.as_opt<ip::aggregate_value>()};
        if (!items) { return stdx::none; }
        return std::vector<stdx::option<ip::index>>{items->elements};
    }};
    const stdx::option<std::string> lhs_copy{
        lhs_text.transform([](const std::string& s) { return s; })};
    const stdx::option<std::string> rhs_copy{
        rhs_text.transform([](const std::string& s) { return s; })};
    const auto lhs_items{items_of(*lhs_known, lhs_copy)};
    const auto rhs_items{items_of(*rhs_known, rhs_copy)};
    if (!lhs_items || !rhs_items) { return comp_.unsupported(u_, src, "a `++` of these values"); }

    std::vector<stdx::option<ip::index>> elements{*lhs_items};
    elements.insert(elements.end(), rhs_items->begin(), rhs_items->end());
    auto& type{ctx.get_array(
        gs::types::mut::CONSTANT, rhs_shape->null_terminated, elements.size(), lhs_shape->elem)};
    return operand{pool.intern(ip::aggregate_value{std::move(elements)}, type)};
}

} // namespace ghoti::uir::sema
