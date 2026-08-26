#include "compiler/syntax/layout_engine.hh"

#include <ostream>
#include <ranges>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/syntax/doc.hh"

namespace ghoti::syntax {

auto layout_engine::render(doc_id root, std::ostream& os) -> void {
    u32                         current_width{0};
    std::vector<layout_command> stack{{
        .doc          = root,
        .indent_level = 0,
        .mode         = layout_mode::FLAT,
    }};

    while (!stack.empty()) {
        auto [doc, indent_level, mode]{stack.back()};
        stack.pop_back();

        manager_[doc].visit(
            [&](docs::text t) {
                fmt::print(os, "{}", t.text);
                current_width += static_cast<u32>(t.text.size());
            },
            [&](const docs::concat& c) {
                // Push in reverse order to process children seq
                for (const auto& child : std::views::reverse(c.children)) {
                    stack.emplace_back(child, indent_level, mode);
                }
            },
            [&](docs::indent i) { stack.emplace_back(i.child, indent_level + i.amount, mode); },
            [&](docs::group g) {
                auto fit_mode{layout_mode::BREAK};
                // Test if the group fits horizontally in flat mode
                if (!g.force_break && fits(current_width, g.child)) {
                    fit_mode = layout_mode::FLAT;
                }
                stack.emplace_back(g.child, indent_level, fit_mode);
            },
            [&](docs::line_or_space l) {
                if (mode == layout_mode::FLAT) {
                    fmt::print(os, "{}", l.space_text);
                    current_width += static_cast<u32>(l.space_text.size());
                } else {
                    fmt::print(os, "\n{:{}}", "", indent_level);
                    current_width = indent_level;
                }
            },
            [&](docs::hard_line) {
                fmt::print(os, "\n{:{}}", "", indent_level);
                current_width = indent_level;
            },
            [&](docs::soft_line) {
                if (mode == layout_mode::BREAK) {
                    fmt::print(os, "\n{:{}}", "", indent_level);
                    current_width = indent_level;
                }
            },
            [&](docs::align a) { stack.emplace_back(a.child, current_width + a.columns, mode); });
    }
}

auto layout_engine::fits(u32 current_width, doc_id doc) const noexcept -> bool {
    auto remaining{static_cast<i64>(max_width_) - static_cast<i64>(current_width)};
    if (remaining < 0) { return false; }
    return measure(doc, remaining);
}

auto layout_engine::measure(doc_id doc, i64& width_left) const noexcept -> bool {
    if (width_left < 0) { return false; }

    return manager_[doc].visit(
        [&](docs::text t) {
            width_left -= static_cast<i64>(t.text.size());
            return width_left >= 0;
        },
        [&](const docs::concat& c) {
            for (auto child : c.children) {
                if (!measure(child, width_left)) { return false; }
            }
            return true;
        },
        [&](docs::indent i) { return measure(i.child, width_left); },
        [&](docs::group g) { return !g.force_break && measure(g.child, width_left); },
        [&](docs::line_or_space l) {
            width_left -= static_cast<i64>(l.space_text.size());
            return width_left >= 0;
        },
        [&](docs::hard_line) { return true; },
        [&](docs::soft_line) { return true; },
        [&](docs::align a) { return measure(a.child, width_left); });
}

} // namespace ghoti::syntax
