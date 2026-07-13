#include "compiler/gir/dumper.hh"

#include <ranges>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

namespace {

auto format_type(const sema::type& type) -> std::string {
    return type.get_data().visit(
        [](sema::types::pointer ptr) { return fmt::format("^{}", format_type(ptr.underlying)); },
        [](sema::types::reference ref) { return fmt::format("&{}", format_type(ref.underlying)); },
        [](sema::types::slice slice) { return fmt::format("[]{}", format_type(slice.underlying)); },
        [](sema::types::array arr) {
            return fmt::format("[{}]{}", arr.len, format_type(arr.underlying));
        },
        [](sema::types::function fn) {
            auto params_str{
                fmt::to_string(fmt::join(fn.params | std::views::transform([](sema::type* param) {
                                             return format_type(*param);
                                         }),
                                         ", "))};
            if (fn.is_variadic) {
                if (!params_str.empty()) {
                    params_str += ", ...";
                } else {
                    params_str = "...";
                }
            }
            return fmt::format("fn({}) -> {}", params_str, format_type(fn.return_type));
        },
        [&type](const auto&) {
            return std::string{sema::type_kind_display_name(type.get_kind())};
        });
}

auto format_value(const value& val) -> std::string {
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
        [](const std::string& str) { return fmt::format("\"{}\"", str); },
        [](stdx::option<sema::type&> t) { return t ? format_type(*t) : "<null_type>"; },
        [](void_val) { return "void"; },
        [](undefined_val) { return "undefined"; },
        [](auto v) { return fmt::format("{}", v); });
}

auto format_instruction(const instruction& inst) -> std::string {
    const auto type_str = inst.type ? format_type(*inst.type) : "";
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
    case instruction_kind::LOAD:
        return fmt::format("{}{} {}",
                           prefix,
                           instruction_kind_name(inst.kind),
                           !inst.operands.empty() ? format_value(inst.operands[0])
                                                  : "<missing_src>");
    case instruction_kind::CONST: {
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
    default: break;
    }

    // Binary / unary / comparison / cast operations
    if (!type_str.empty() && !inst.operands.empty()) {
        return fmt::format("{}{} {} {}",
                           prefix,
                           instruction_kind_name(inst.kind),
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

auto dumper::dump(const instruction& inst) -> void {
    fmt::println(out_, "    {}", format_instruction(inst));
}

auto dumper::dump(const segment& seg) -> void {
    fmt::println(out_, "  seg {}:", std::to_underlying(seg.get_id()));
    for (const auto& inst : seg.get_instructions()) { dump(*inst); }
}

auto dumper::dump(const function& fn) -> void {
    if (fn.get_is_test()) {
        fmt::println(out_, "test \"{}\"", fn.get_name());
    } else {
        std::string return_str;
        if (const auto fn_type{fn.get_type().get_data().as_opt<sema::types::function>()}) {
            return_str = format_type(fn_type->return_type);
        } else {
            return_str = format_type(fn.get_type());
        }

        auto params_str{fmt::to_string(
            fmt::join(fn.get_params() | std::views::transform([](const parameter* p) {
                          return fmt::format("{}: {}", p->name, format_type(p->type));
                      }),
                      ", "))};
        if (fn.get_is_variadic()) {
            if (!params_str.empty()) {
                params_str += ", ...";
            } else {
                params_str = "...";
            }
        }

        fmt::println(out_, "fn {}({}) -> {}", fn.get_name(), params_str, return_str);
    }

    for (const auto& seg : fn.get_segments()) { dump(*seg); }
}

auto dumper::dump(const module& mod) -> void {
    for (const auto& type_decl : mod.get_types()) {
        fmt::println(out_, "type {} = {}", type_decl->name, format_type(type_decl->type));
    }

    for (const auto& global : mod.get_globals()) {
        const auto kw{global->is_constant ? "const" : "var"};
        if (global->init_value) {
            fmt::println(out_,
                         "{} {}: {} = {}",
                         kw,
                         global->name,
                         format_type(global->type),
                         format_value(*global->init_value));
        } else {
            fmt::println(out_, "{} {}: {}", kw, global->name, format_type(global->type));
        }
    }

    for (const auto& fn : mod.get_functions()) {
        dump(*fn);
        fmt::println(out_, "");
    }
}

auto dumper::dump(const value& val) -> void { fmt::print(out_, "{}", format_value(val)); }

} // namespace ghoti::gir
