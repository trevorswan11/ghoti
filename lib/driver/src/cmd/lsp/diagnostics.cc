#include "driver/cmd/lsp/diagnostics.hh"

#include <stdx/variant.hh>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "driver/cmd/lsp/text_edit.hh"
#include "support/diagnostic.hh"
#include "support/path_utils.hh"

namespace ghoti::lsp {

namespace {

// LSP DiagnosticSeverity: 1 = Error, 2 = Warning; absent lets the client pick its own default
[[nodiscard]] auto severity_of(stdx::option<diagnostic_level> level) -> stdx::option<i32> {
    return level.transform([](diagnostic_level l) { return static_cast<i32>(l); });
}

auto push_diagnostic(nlohmann::json& out, const auto& d, std::string_view text) -> void {
    const auto     formattable{d.to_formattable()};
    nlohmann::json entry{
        {"range", range_of(formattable.location, text)},
        {"message", formattable.message.value_or(std::string{formattable.error_name})},
        {"source", "ghoti"},
        {"code", std::string{formattable.error_name}},
    };
    if (const auto severity{severity_of(formattable.level)}) { entry["severity"] = *severity; }
    out.push_back(std::move(entry));
}

auto push_diagnostic(nlohmann::json& out, const diagnostic_snapshot& d, std::string_view text)
    -> void {
    out.push_back({
        {"range", range_of(d.location, text)},
        {"message", d.message},
        {"source", "ghoti"},
        {"code", d.error_name},
        {"severity", static_cast<i32>(d.level)},
    });
}

} // namespace

auto to_lsp_diagnostics(const mod::module_t& module) -> nlohmann::json {
    auto out = nlohmann::json::array();

    // module.parse_diagnostics is a permanent record of syntax errors found at parse time
    const std::string_view text{module.source};
    for (const auto& d : module.parse_diagnostics) { push_diagnostic(out, d, text); }
    if (const auto sema_diags{module.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *sema_diags) {
            const auto origin{d.origin()};
            if (!origin || origin.get() == &module) {
                push_diagnostic(out, d, text);
                continue;
            }
            // An error inside another module's code is that file's to show, if it shows it
            if (origin->reports(d)) { continue; }
            push_diagnostic(out, d, text);
            const auto loc{d.to_formattable().location.value_or(source_location{0, 0})};
            out.back()["range"]   = range_of(source_location{0, 0}, text);
            out.back()["message"] = fmt::format("{}:{}:{}: {}",
                                                path_utils::display(origin->path),
                                                loc.line + 1,
                                                loc.column + 1,
                                                out.back()["message"].get<std::string>());
        }
    }
    for (const auto& warning : module.warnings) {
        push_diagnostic(out, warning, text);
        // LSP DiagnosticTag 2 = Deprecated, which clients render as a strikethrough
        if (warning.get_error() == sema::error::DEPRECATED_USE) { out.back()["tags"] = {2}; }
    }

    return out;
}

auto range_of(stdx::option<source_location> loc, std::string_view text) -> nlohmann::json {
    const auto start{loc.value_or(source_location{0, 0})};
    return {
        {"start", client_position(text, start)},
        {"end", client_position(text, {start.line, start.column + 1})},
    };
}

auto range_of(source_span span, std::string_view text) -> nlohmann::json {
    return {
        {"start", client_position(text, span.start)},
        {"end", client_position(text, span.end)},
    };
}

} // namespace ghoti::lsp
