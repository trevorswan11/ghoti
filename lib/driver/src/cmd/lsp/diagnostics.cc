#include "driver/cmd/lsp/diagnostics.hh"

#include <stdx/variant.hh>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::lsp {

namespace {

// LSP DiagnosticSeverity: 1 = Error, 2 = Warning; absent lets the client pick its own default
[[nodiscard]] auto severity_of(stdx::option<diagnostic_level> level) -> stdx::option<i32> {
    return level.transform([](diagnostic_level l) { return static_cast<i32>(l); });
}

auto push_diagnostic(nlohmann::json& out, const auto& d) -> void {
    const auto     formattable{d.to_formattable()};
    nlohmann::json entry{
        {"range", range_of(formattable.location)},
        {"message", formattable.message.value_or(std::string{formattable.error_name})},
        {"source", "ghoti"},
        {"code", std::string{formattable.error_name}},
    };
    if (const auto severity{severity_of(formattable.level)}) { entry["severity"] = *severity; }
    out.push_back(std::move(entry));
}

auto push_diagnostic(nlohmann::json& out, const diagnostic_snapshot& d) -> void {
    out.push_back({
        {"range", range_of(d.location)},
        {"message", d.message},
        {"source", "ghoti"},
        {"code", d.error_name},
        {"severity", static_cast<i32>(d.level)},
    });
}

} // namespace

auto to_lsp_diagnostics(const mod::module& module) -> nlohmann::json {
    auto out = nlohmann::json::array();

    // module.parse_diagnostics is a permanent record of syntax errors found at parse time
    for (const auto& d : module.parse_diagnostics) { push_diagnostic(out, d); }
    if (const auto sema_diags{module.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *sema_diags) { push_diagnostic(out, d); }
    }

    return out;
}

auto range_of(stdx::option<source_location> loc) -> nlohmann::json {
    const auto start{loc.value_or(source_location{0, 0})};
    return {
        {
            "start",
            {
                {"line", start.line},
                {"character", start.column},
            },
        },
        {
            "end",
            {
                {"line", start.line},
                {"character", start.column + 1},
            },
        },
    };
}

auto range_of(source_span span) -> nlohmann::json {
    return {
        {
            "start",
            {
                {"line", span.start.line},
                {"character", span.start.column},
            },
        },
        {
            "end",
            {
                {"line", span.end.line},
                {"character", span.end.column},
            },
        },
    };
}

} // namespace ghoti::lsp
