#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

[[nodiscard]] auto has_option(u32 bits, ast::asm_expr::option opt) noexcept -> bool {
    return (bits & (1U << static_cast<u32>(opt))) != 0;
}

} // namespace

auto analyzer::analyze_asm(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    TRY(require_runtime(src));
    const auto tmpl{rec.str_field()};
    const auto result_type_ref{rec.ref_field()};

    std::vector<std::pair<std::string_view, stdx::option<ref>>> outputs;
    for (u32 i{0}, count{rec.count()}; i < count; ++i) {
        const auto text{rec.str_field()};
        outputs.emplace_back(text, rec.ref_field());
    }

    std::vector<std::pair<std::string_view, stdx::option<ref>>> inputs;
    for (u32 i{0}, count{rec.count()}; i < count; ++i) {
        const auto text{rec.str_field()};
        inputs.emplace_back(text, rec.ref_field());
    }

    std::vector<std::string_view> clobbers;
    for (u32 i{0}, count{rec.count()}; i < count; ++i) { clobbers.emplace_back(rec.str_field()); }
    const auto bits{rec.u32_field()};

    gir::inline_asm info;
    info.is_volatile   = has_option(bits, ast::asm_expr::option::VOLATILE);
    info.is_noreturn   = has_option(bits, ast::asm_expr::option::NORETURN);
    info.align_stack   = has_option(bits, ast::asm_expr::option::ALIGN_STACK);
    info.intel_dialect = has_option(bits, ast::asm_expr::option::INTEL);
    info.tmpl          = gir::asm_template(tmpl);

    // The constraint list names the outputs, then the inputs, then the clobbers
    std::vector<std::string> pieces;
    for (const auto& [text, value] : outputs) { pieces.emplace_back(text); }
    for (const auto& [text, value] : inputs) { pieces.emplace_back(text); }
    for (const auto clobber : clobbers) { pieces.emplace_back(fmt::format("~{{{}}}", clobber)); }
    info.constraints = fmt::format("{}", fmt::join(pieces, ","));

    std::vector<gir::value> values;
    values.reserve(inputs.size());
    for (const auto& [text, value] : inputs) {
        if (!value) { return comp_.unsupported(u_, src, "an `asm` input without a value"); }
        const auto given{TRY(resolve_value(*value, src))};
        values.emplace_back(TRY(as_runtime(given, src)));
    }

    // An output written to a place gets its address, and `_` is the asm's own result
    stdx::option<gs::type&> result_type;
    for (const auto& [text, value] : outputs) {
        if (!value) {
            info.has_result_slot = true;
            if (!result_type_ref) {
                return comp_.unsupported(u_, src, "an `asm` result without a type");
            }

            result_type.emplace(*TRY(resolve_type(*result_type_ref, src)));
            continue;
        }

        info.output_addrs.emplace_back(TRY(address_of(*value, src)).address);
    }

    auto&      void_type{builtin(gs::type_kind::VOID_)};
    auto&      asm_type{result_type.value_or(void_type)};
    const bool noreturn{info.is_noreturn};
    const auto dest{builder_.emit_inline_asm(std::move(info), std::move(values), asm_type)};
    if (noreturn) {
        builder_.emit_unreachable();
        return comp_.constants().intern(ip::undefined_value{}, builtin(gs::type_kind::NORETURN));
    }

    if (dest) { return gir::value{*dest, asm_type}; }
    return void_value();
}

} // namespace ghoti::uir::sema
