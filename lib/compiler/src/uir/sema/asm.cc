#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <llvm/IR/InlineAsm.h>
#include <stdx/enum.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace {

[[nodiscard]] auto has_option(u32 bits, ast::asm_expr::option opt) noexcept -> bool {
    return (bits & (1U << static_cast<u32>(opt))) != 0;
}

// The written node a source names, its own location when it isn't a node
[[nodiscard]] auto node_source(ast::node_id id) -> source { return source{id}; }

} // namespace

auto analyzer::check_asm_form(const source& src) -> result<void> {
    const auto id{src.as_opt<ast::node_id>()};
    if (!id) { return {}; }
    const auto& node{u_.source->ast.get_as<ast::asm_expr>(*id)};
    const auto& ast{u_.source->ast};
    const auto  fail{[&](const source& at, std::string message) -> result<void> {
        return comp_.report(u_, at, gs::error::ILLEGAL_INLINE_ASM, std::move(message));
    }};

    // Only an output may be discarded, since an input has to pass something in
    for (const auto& op : node.inputs) {
        if (!op.value) {
            return fail(node_source(op.constraint),
                        "An asm input operand needs a value; only outputs may be `_`");
        }
    }

    // Each operand constraint is exactly one well-formed LLVM constraint of its direction
    const auto check_constraint{[&](ast::string_handle constraint, bool is_output) -> result<void> {
        const auto text{ast.get_as<ast::string_expr>(constraint).value};
        const auto parsed{llvm::InlineAsm::ParseConstraints(text)};
        const bool well_formed{parsed.size() == 1 &&
                               (parsed.front().Type == llvm::InlineAsm::isOutput) == is_output &&
                               parsed.front().Type != llvm::InlineAsm::isClobber};
        if (well_formed) { return {}; }
        return fail(
            node_source(constraint),
            fmt::format("Invalid asm {} constraint \"{}\"", is_output ? "output" : "input", text));
    }};
    for (const auto& op : node.outputs) { TRY(check_constraint(op.constraint, true)); }
    for (const auto& op : node.inputs) { TRY(check_constraint(op.constraint, false)); }

    usize bound_outputs{0};
    usize result_slots{0};
    for (const auto& op : node.outputs) {
        if (!op.value) {
            ++result_slots;
            continue;
        }

        // An output names something it can store to
        ++bound_outputs;
        const auto kind{(*op.value)->get_kind()};
        const bool assignable{kind == ast::node_kind::IDENTIFIER_EXPRESSION ||
                              kind == ast::node_kind::DOT_EXPRESSION ||
                              kind == ast::node_kind::INDEX_EXPRESSION ||
                              kind == ast::node_kind::DEREFERENCE_EXPRESSION ||
                              kind == ast::node_kind::IMPLICIT_ACCESS_EXPRESSION};
        if (!assignable) {
            return fail(node_source(*op.value), "An asm output operand must be an assignable lvalue");
        }
    }

    const bool has_volatile{node.has_option(ast::asm_expr::option::VOLATILE)};
    const bool has_noreturn{node.has_option(ast::asm_expr::option::NORETURN)};
    for (const auto opt : stdx::enum_range<ast::asm_expr::option>()) {
        if (std::ranges::count(node.options, opt) > 1) { return fail(src, "Duplicate asm option"); }
    }
    if (node.has_option(ast::asm_expr::option::INTEL) &&
        node.has_option(ast::asm_expr::option::ATT)) {
        return fail(src, "The asm options 'intel' and 'att' are mutually exclusive");
    }

    // `volatile` is never implied, so an asm with no outputs states it or has no effect
    if (node.outputs.empty() && !has_volatile) {
        return fail(src, "An asm block with no output operands must be marked `volatile`");
    }
    if (result_slots > 1) { return fail(src, "An asm block may have at most one `_` result operand"); }
    if (result_slots == 1 && !node.result_type) {
        return fail(src,
                    "An asm block with a `_` result operand needs an explicit result type, e.g. "
                    "`asm u32 { ... }`");
    }
    if (result_slots == 1 && node.outputs.size() != 1) {
        return fail(src, "An asm block with a `_` result operand may not bind any other outputs");
    }
    if (result_slots == 0 && node.result_type) {
        return fail(src, "An asm result type is only meaningful alongside a `_` result operand");
    }
    if (has_noreturn && (bound_outputs > 0 || result_slots > 0)) {
        return fail(src, "A `noreturn` asm block cannot bind any output operands");
    }

    // A `%N` placeholder indexes an operand, the outputs numbered before the inputs
    const auto& tmpl{ast.get_as<ast::string_expr>(node.tmpl).value};
    const usize operands{node.outputs.size() + node.inputs.size()};
    for (usize i{0}; i + 1 < tmpl.size(); ++i) {
        if (tmpl[i] != '%') { continue; }
        if (tmpl[i + 1] == '%') {
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(tmpl[i + 1])) == 0) { continue; }
        usize index{0};
        usize j{i + 1};
        for (; j < tmpl.size() && std::isdigit(static_cast<unsigned char>(tmpl[j])) != 0; ++j) {
            index = (index * 10) + static_cast<usize>(tmpl[j] - '0');
        }
        i = j - 1;
        if (index >= operands) {
            return fail(src,
                        fmt::format("asm template references operand %{} but {} operand(s) are "
                                    "given (outputs are numbered first, then inputs)",
                                    index,
                                    operands));
        }
    }

    return {};
}

auto analyzer::analyze_asm(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    TRY(require_runtime(src));
    TRY(check_asm_form(src));
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
        if (!value) {
            return comp_.report(u_,
                                src,
                                gs::error::ILLEGAL_INLINE_ASM,
                                fmt::format("asm input '{}' needs a value; only an output can be "
                                            "'_'",
                                            text));
        }
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
