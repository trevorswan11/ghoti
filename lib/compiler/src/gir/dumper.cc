#include "compiler/gir/dumper.hh"

#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <gsl/util>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/keywords.hh"

namespace ghoti::gir {

namespace {

auto format_constant(const ip::pool& constants, ip::index at) -> std::string {
    if (constants.is_type(at)) { return constants.type_at(at).to_string(); }
    return constants.value_at(at).data.visit(
        [](const std::string& str) { return fmt::format("\"{}\"", str); },
        [](ip::no_type_value) -> std::string { return "<null_type>"; },
        [](ip::void_value) -> std::string { return "void"; },
        [](ip::undefined_value) -> std::string { return "undefined"; },
        [](ip::zero_value) -> std::string { return "zero"; },
        [](ip::nullptr_value) -> std::string { return "nullptr"; },
        [](const ip::aggregate_value&) -> std::string {
            UNREACHABLE("An aggregate constant is never an operand");
        },
        [](const ip::enum_value&) -> std::string {
            UNREACHABLE("An enum constant is never an operand");
        },
        [](const ip::union_value&) -> std::string {
            UNREACHABLE("A union constant is never an operand");
        },
        [](const ip::address_value&) -> std::string {
            UNREACHABLE("An address constant is never an operand");
        },
        [](const ip::dyn_value&) -> std::string {
            UNREACHABLE("A dyn constant is never an operand");
        },
        [](ip::poison_value) -> std::string {
            UNREACHABLE("A poison constant is never an operand");
        },
        [](const auto& v) { return fmt::format("{}", v); });
}

auto format_value(const ip::pool& constants, const value& val) -> std::string {
    return val.data.visit(
        [](local_id loc) {
            switch (loc.get_kind()) {
            case local_kind::TEMPORARY: return fmt::format("%{}", loc.get_index());
            case local_kind::PARAMETER: return fmt::format("param.{}", loc.get_index());
            case local_kind::ALLOCA:    return fmt::format("%{}", loc.get_index());
            case local_kind::GLOBAL:    return fmt::format("@{}", loc.get_index());
            default:                    UNREACHABLE("Unknown local ID");
            }
        },
        [&constants](ip::index at) { return format_constant(constants, at); });
}

auto linkage_name(gir::linkage l) noexcept -> std::string_view {
    switch (l) {
    case linkage::INTERNAL: return "internal";
    case linkage::PUBLIC:   return "public";
    case linkage::EXPORT:   return "export";
    case linkage::EXTERN:   return "extern";
    default:                return "internal";
    }
}

auto format_instruction(const ip::pool& constants, const instruction& inst, bool show_flags)
    -> std::string {
    const auto format_value{
        [&constants](const value& val) { return gir::format_value(constants, val); }};
    const auto type_str = inst.type ? inst.type->to_string() : "";
    const auto prefix   = inst.result ? fmt::format("%{} = ", inst.result->get_index()) : "";

    switch (inst.kind) {
    case instruction_kind::RET:
        if (inst.operands.empty()) { return "ret void"; }
        if (!type_str.empty()) {
            return fmt::format("ret {} {}", type_str, format_value(inst.operands[0]));
        }
        return fmt::format("ret {}", format_value(inst.operands[0]));
    case instruction_kind::GOTO:
        return fmt::format(
            "goto seg {}",
            inst.target_segment.transform([](auto s) { return std::to_underlying(s); })
                .value_or(0));
    case instruction_kind::COND_GOTO:
        return fmt::format(
            "cond_goto {} seg {}, seg {}",
            !inst.operands.empty() ? format_value(inst.operands[0]) : "<missing_cond>",
            inst.true_segment.transform([](auto s) { return std::to_underlying(s); }).value_or(0),
            inst.false_segment.transform([](auto s) { return std::to_underlying(s); }).value_or(0));
    case instruction_kind::UNREACHABLE: return "unreachable";
    case instruction_kind::STORE:
        if (inst.operands.size() >= 2) {
            return fmt::format(
                "store {}, {}", format_value(inst.operands[0]), format_value(inst.operands[1]));
        } else if (inst.operands.size() == 1 && inst.result) {
            return fmt::format(
                "store {}, %{}", format_value(inst.operands[0]), inst.result->get_index());
        }
        return fmt::format("store {}, {}",
                           inst.operands.size() > 0 ? format_value(inst.operands[0])
                                                    : "<missing_src>",
                           "<missing_dst>");
    case instruction_kind::ALLOCA:
        return fmt::format("{}{} {}", prefix, instruction_kind_name(inst.kind), type_str);
    case instruction_kind::MAKE_CALLABLE:
        return fmt::format("{}{} {} {}, @{}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           type_str,
                           !inst.operands.empty() ? format_value(inst.operands[0]) : "<missing>",
                           inst.callee_name.value_or("<trampoline>"));
    case instruction_kind::GLOBAL_ADDR:
        return fmt::format("{}{} @{}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           inst.callee_name.value_or("<missing_global>"));
    case instruction_kind::LOAD:
        return fmt::format("{}{} {}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           !inst.operands.empty() ? format_value(inst.operands[0])
                                                  : "<missing_src>");
    case instruction_kind::CONSTANT: {
        const auto val{!inst.operands.empty() ? format_value(inst.operands[0]) : "<missing_val>"};
        if (!type_str.empty()) {
            return fmt::format(
                "{}{} {} {}", prefix, instruction_kind_name(inst.kind), type_str, val);
        }
        return fmt::format("{}{} {}", prefix, instruction_kind_name(inst.kind), val);
    }
    case instruction_kind::CALL:
        if (inst.callee_name) {
            return fmt::format(
                "{}{} @{}({})",
                prefix,
                instruction_kind_name(inst.kind),
                *inst.callee_name,
                fmt::join(inst.operands | std::views::transform(format_value), ", "));
        }

        return fmt::format(
            "{}{} {}({})",
            prefix,
            instruction_kind_name(inst.kind),
            !inst.operands.empty() ? format_value(inst.operands[0]) : "<missing_callee>",
            fmt::join(inst.operands | std::views::drop(1) | std::views::transform(format_value),
                      ", "));
    case instruction_kind::BUILTIN_CALL:
        return fmt::format("{}{} @{}({})",
                           prefix,
                           instruction_kind_name(inst.kind),
                           inst.callee_name.value_or("builtin"),
                           fmt::join(inst.operands | std::views::transform(format_value), ", "));
    case instruction_kind::INLINE_ASM: {
        const auto& info{inst.asm_info};
        std::string flags;
        if (info) {
            if (info->is_volatile) { flags += " volatile"; }
            if (info->is_noreturn) { flags += " noreturn"; }
            if (info->align_stack) { flags += " alignstack"; }
            if (info->intel_dialect) { flags += " inteldialect"; }
        }
        return fmt::format("{}{}{} \"{}\" \"{}\"({})",
                           prefix,
                           instruction_kind_name(inst.kind),
                           flags,
                           info ? info->tmpl : std::string{},
                           info ? info->constraints : std::string{},
                           fmt::join(inst.operands | std::views::transform(format_value), ", "));
    }
    default: break;
    }

    // Binary / unary / comparison / cast operations
    const auto sat{fmt::format(
        "{}{}", inst.is_saturating ? ".sat" : "", show_flags && inst.is_checked ? ".checked" : "")};
    if (!type_str.empty() && !inst.operands.empty()) {
        return fmt::format("{}{}{} {} {}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           sat,
                           type_str,
                           fmt::join(inst.operands | std::views::transform(format_value), ", "));
    }

    if (!inst.operands.empty()) {
        return fmt::format("{}{} {}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           fmt::join(inst.operands | std::views::transform(format_value), ", "));
    }
    return fmt::format("{}{}", prefix, instruction_kind_name(inst.kind));
}

} // namespace

auto dumper::constants() const -> const ip::pool& {
    ASSERT(constants_, "Dumping GIR outside a module or function needs the intern pool");
    return *constants_;
}

auto dumper::dump(const instruction& inst) -> void {
    fmt::println(out_, "    {}", format_instruction(constants(), inst, show_flags_));
}

auto dumper::dump(const segment& seg) -> void {
    fmt::println(out_, "  seg {}:", std::to_underlying(seg.get_id()));
    for (const auto& inst : seg.get_instructions()) { dump(*inst); }
}

auto dumper::dump(const function& fn) -> void {
    const auto outer{std::exchange(constants_, fn.constants())};
    const auto restore{gsl::finally([&] { constants_ = outer; })};
    if (fn.get_is_test()) {
        const auto test_name{fn.get_test_desc().empty() ? fn.get_name() : fn.get_test_desc()};
        fmt::println(out_, "test \"{}\"", test_name);
    } else {
        std::string return_str;
        if (const auto fn_type{fn.get_type().get_data().as_opt<sema::types::function>()}) {
            return_str = fn_type->return_type.to_string();
        } else {
            return_str = fn.get_type().to_string();
        }

        auto params_str{fmt::to_string(
            fmt::join(fn.get_params() |
                          std::views::transform([](gsl::not_null<const function::parameter*> p) {
                              return fmt::format("{}: {}", p->name, p->type.to_string());
                          }),
                      ", "))};
        if (fn.get_is_variadic()) {
            if (!params_str.empty()) {
                params_str += ", ...";
            } else {
                params_str = "...";
            }
        }

        fmt::println(out_,
                     "fn {}({}) -> {}{}",
                     syntax::ascii_spelling(fn.get_name()),
                     params_str,
                     return_str,
                     show_flags_ ? fmt::format(" [{}]", linkage_name(fn.get_linkage())) : "");
    }

    for (const auto& seg : fn.get_segments()) { dump(*seg); }
}

auto dumper::dump(const module_t& mod) -> void {
    const auto outer{std::exchange(constants_, mod.constants())};
    const auto restore{gsl::finally([&] { constants_ = outer; })};
    for (const auto& type_decl : mod.get_types()) {
        fmt::println(out_, "type {} = {}", type_decl->name, type_decl->type.to_string());
    }

    for (const auto& global : mod.get_globals()) {
        const auto kw{global->is_constant ? "const" : "var"};
        const auto flags{show_flags_ ? fmt::format(" [{}]", linkage_name(global->linkage)) : ""};
        if (global->init_value) {
            fmt::println(out_,
                         "{} {}: {} = {}{}",
                         kw,
                         global->name,
                         global->type.to_string(),
                         format_value(constants(), *global->init_value),
                         flags);
        } else {
            fmt::println(out_, "{} {}: {}{}", kw, global->name, global->type.to_string(), flags);
        }
    }

    for (const auto& fn : mod.get_functions()) {
        dump(*fn);
        fmt::println(out_, "");
    }
}

auto dumper::dump(const value& val) -> void {
    fmt::print(out_, "{}", format_value(constants(), val));
}

} // namespace ghoti::gir
