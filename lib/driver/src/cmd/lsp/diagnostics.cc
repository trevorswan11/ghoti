#include "driver/cmd/lsp/diagnostics.hh"

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

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

class diagnostic_converter {
  public:
    diagnostic_converter(const mod::module_t& module, const source_text_lookup& lookup)
        : module_{module}, text_{module.source}, lookup_{lookup} {}

    // `origin` is the file the primary span points into, when it isn't this module
    [[nodiscard]] auto convert(const detail::formattable_diagnostic& d,
                               stdx::option<const mod::module_t&>    origin = stdx::none) const
        -> nlohmann::json {
        std::string message{d.message};
        auto        related = nlohmann::json::array();
        if (d.span && !d.primary_label.empty()) {
            const auto& primary_path{origin ? origin->path : module_.path};
            related.push_back(related_entry(primary_path, *d.span, d.primary_label));
        }
        for (const auto& label : d.labels) {
            related.push_back(related_entry(label.at.path, label.at.span, label.text));
        }
        for (const auto& note : d.notes) {
            if (note.at) {
                related.push_back(related_entry(note.at->path, note.at->span, note.text));
            } else {
                message += fmt::format("\nnote: {}", note.text);
            }
        }

        nlohmann::json entry{
            {"range", d.span ? range_of(*d.span, text_) : range_of(stdx::none, text_)},
            {"message", std::move(message)},
            {"source", "ghoti"},
            {"code", std::string{d.error_name}},
        };
        if (const auto severity{severity_of(d.level)}) { entry["severity"] = *severity; }
        if (!related.empty()) { entry["relatedInformation"] = std::move(related); }
        return entry;
    }

  private:
    [[nodiscard]] auto related_entry(const std::filesystem::path& path,
                                     source_span                  span,
                                     std::string_view             message) const -> nlohmann::json {
        const auto text{path == module_.path ? stdx::option<std::string_view>{text_}
                        : lookup_            ? lookup_(path)
                                             : stdx::option<std::string_view>{}};
        return {
            {"location",
             {{"uri", path_utils::path_to_uri(path)},
              {"range", range_of(span, text.value_or(std::string_view{}))}}},
            {"message", std::string{message}},
        };
    }

  private:
    const mod::module_t&      module_;
    std::string_view          text_;
    const source_text_lookup& lookup_;
};

// Sorted like the terminal output, so the order analysis ran in never shows
auto sort_by_position(nlohmann::json& diagnostics) -> void {
    const auto key{[](const nlohmann::json& d) {
        const auto& start{d.at("range").at("start")};
        return std::tuple{start.at("line").get<i64>(),
                          start.at("character").get<i64>(),
                          d.at("code").get<std::string>(),
                          d.at("message").get<std::string>()};
    }};
    std::stable_sort(
        diagnostics.begin(),
        diagnostics.end(),
        [&key](const nlohmann::json& a, const nlohmann::json& b) { return key(a) < key(b); });
}

} // namespace

auto to_lsp_diagnostics(const mod::module_t& module, const source_text_lookup& lookup)
    -> nlohmann::json {
    auto                       out = nlohmann::json::array();
    const diagnostic_converter converter{module, lookup};

    // module.parse_diagnostics is a permanent record of syntax errors found at parse time
    for (const auto& d : module.parse_diagnostics) {
        out.push_back(converter.convert(d.to_formattable()));
    }
    if (const auto sema_diags{module.diagnostics.as_opt<sema::diagnostics>()}) {
        for (const auto& d : *sema_diags) {
            const auto origin{d.origin()};
            if (!origin || origin.get() == &module) {
                out.push_back(converter.convert(d.to_formattable()));
                continue;
            }
            // An error inside another module's code is that file's to show, if it shows it
            if (origin->reports(d)) { continue; }
            out.push_back(converter.convert(d.to_formattable(), *origin));
            const auto loc{d.location().value_or(source_location{0, 0})};
            out.back()["range"] = range_of(source_location{0, 0}, std::string_view{module.source});
            out.back()["message"] = fmt::format("{}:{}:{}: {}",
                                                path_utils::display(origin->path),
                                                loc.line + 1,
                                                loc.column + 1,
                                                out.back()["message"].get<std::string>());
        }
    }
    for (const auto& warning : module.warnings) {
        out.push_back(converter.convert(warning.to_formattable()));
        // LSP DiagnosticTag 2 = Deprecated, which clients render as a strikethrough
        if (warning.get_error() == sema::error::DEPRECATED_USE) { out.back()["tags"] = {2}; }
    }

    sort_by_position(out);
    return out;
}

auto range_of(stdx::option<source_location> loc, std::string_view text) -> nlohmann::json {
    return range_of(source_span::point(loc.value_or(source_location{0, 0})), text);
}

auto range_of(source_span span, std::string_view text) -> nlohmann::json {
    return {
        {"start", client_position(text, span.start)},
        {"end", client_position(text, span.end)},
    };
}

} // namespace ghoti::lsp
