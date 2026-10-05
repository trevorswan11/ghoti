#include "compiler/syntax/layout_engine.hh"

#include <ostream>
#include <ranges>
#include <vector>

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/syntax/doc.hh"
#include "support/unicode.hh"

namespace ghoti::syntax {

auto layout_engine::render(std::ostream& os) -> void {
    for (const auto root : doc_manager_) { render(root, os); }
}

auto layout_engine::render(doc_id root, std::ostream& os) -> void {
    u32                         current_width{0};
    u16                         pending_cols{0};
    std::vector<layout_command> stack{{
        .doc         = root,
        .indent_cols = 0,
        .mode        = layout_mode::FLAT,
    }};

    const auto flush_indent = [&] {
        if (pending_cols == 0) { return; }
        fmt::print(os, "{:{}}", "", pending_cols);
        pending_cols = 0;
    };

    const auto break_line = [&](u16 cols) {
        fmt::print(os, "\n");
        pending_cols  = cols;
        current_width = cols;
    };

    while (!stack.empty()) {
        auto [doc, indent_cols, mode]{stack.back()};
        stack.pop_back();

        doc_manager_[doc].visit(
            [&](docs::text t) {
                flush_indent();
                fmt::print(os, "{}", t.text);
                current_width += static_cast<u32>(display_width(t.text));
            },
            [&](const docs::concat& c) {
                // Push in reverse order to process children seq
                for (const auto& child : c.children | std::views::reverse) {
                    stack.emplace_back(child, indent_cols, mode);
                }
            },
            [&](docs::indent i) {
                stack.emplace_back(i.child, static_cast<u16>(indent_cols + indent_spaces_), mode);
            },
            [&](docs::group g) {
                auto fit_mode{layout_mode::BREAK};
                // Test if the group fits horizontally in flat mode
                if (!g.force_break && fits(current_width, g.child, stack)) {
                    fit_mode = layout_mode::FLAT;
                }
                stack.emplace_back(g.child, indent_cols, fit_mode);
            },
            [&](docs::line_or_space l) {
                if (mode == layout_mode::FLAT) {
                    flush_indent();
                    fmt::print(os, "{}", l.space_text);
                    current_width += static_cast<u32>(l.space_text.size());
                } else {
                    break_line(indent_cols);
                }
            },
            [&](docs::hard_line) { break_line(indent_cols); },
            [&](docs::soft_line) {
                if (mode == layout_mode::BREAK) { break_line(indent_cols); }
            },
            [&](docs::if_break b) {
                stack.emplace_back(
                    mode == layout_mode::BREAK ? b.when_broken : b.when_flat, indent_cols, mode);
            },
            [&](docs::align a) {
                stack.emplace_back(a.child, static_cast<u16>(current_width + a.columns), mode);
            },
            [&](docs::line_suffix s) { stack.emplace_back(s.child, indent_cols, mode); });
    }
}

auto layout_engine::fits(u32                             current_width,
                         doc_id                          doc,
                         gsl::span<const layout_command> rest) const noexcept -> bool {
    auto remaining{static_cast<i64>(max_width_) - static_cast<i64>(current_width)};
    if (remaining < 0 || !measure(doc, remaining)) { return false; }
    for (const auto& command : rest | std::views::reverse) {
        if (const auto done{measure_until_break(command.doc, command.mode, remaining)}) {
            return *done;
        }
    }
    return true;
}

auto layout_engine::measure_until_break(doc_id      doc,
                                        layout_mode mode,
                                        i64& width_left) const noexcept -> stdx::option<bool> {
    const auto consumed{[&](i64 width) -> stdx::option<bool> {
        width_left -= width;
        if (width_left < 0) { return false; }
        return stdx::none;
    }};
    return doc_manager_[doc].visit(
        [&](docs::text t) { return consumed(static_cast<i64>(display_width(t.text))); },
        [&](const docs::concat& c) -> stdx::option<bool> {
            for (auto child : c.children) {
                if (const auto done{measure_until_break(child, mode, width_left)}) { return done; }
            }
            return stdx::none;
        },
        [&](docs::indent i) { return measure_until_break(i.child, mode, width_left); },
        [&](docs::group g) {
            return measure_until_break(
                g.child, g.force_break ? layout_mode::BREAK : mode, width_left);
        },
        [&](docs::line_or_space l) -> stdx::option<bool> {
            if (mode == layout_mode::BREAK) { return true; }
            return consumed(static_cast<i64>(l.space_text.size()));
        },
        [&](docs::hard_line) -> stdx::option<bool> { return true; },
        [&](docs::soft_line) -> stdx::option<bool> {
            if (mode == layout_mode::BREAK) { return true; }
            return stdx::none;
        },
        [&](docs::if_break b) {
            return measure_until_break(
                mode == layout_mode::BREAK ? b.when_broken : b.when_flat, mode, width_left);
        },
        [&](docs::align a) { return measure_until_break(a.child, mode, width_left); },
        [&](docs::line_suffix) -> stdx::option<bool> { return stdx::none; });
}

auto layout_engine::measure(doc_id doc, i64& width_left) const noexcept -> bool {
    if (width_left < 0) { return false; }

    return doc_manager_[doc].visit(
        [&](docs::text t) {
            width_left -= static_cast<i64>(display_width(t.text));
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
        [&](docs::hard_line) { return false; },
        [&](docs::soft_line) { return true; },
        [&](docs::if_break b) { return measure(b.when_flat, width_left); },
        [&](docs::align a) { return measure(a.child, width_left); },
        [&](docs::line_suffix s) { return measure(s.child, width_left); });
}

} // namespace ghoti::syntax
