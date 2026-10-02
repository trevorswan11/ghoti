#include "compiler/syntax/doc.hh"

#include <algorithm>
#include <cstring>
#include <fmt/format.h>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/types.hh>

namespace ghoti::syntax {

auto doc_manager::text(std::string_view s) -> doc_id { return add<docs::text>(s); }

auto doc_manager::owned(const std::string& s) -> doc_id {
    auto raw{owned_.make_span<char>(s.size())};
    std::memcpy(raw.data(), s.data(), s.size());
    return text(std::string_view{raw.data(), raw.size()});
}

auto doc_manager::concat(std::vector<doc_id> parts) -> doc_id {
    return add<docs::concat>(std::move(parts));
}

auto doc_manager::group(doc_id child, bool force_break) -> doc_id {
    return add<docs::group>(child, force_break);
}

auto doc_manager::nest(doc_id child) -> doc_id { return add<docs::indent>(child); }

auto doc_manager::line() -> doc_id { return add<docs::line_or_space>(std::string_view{" "}); }

auto doc_manager::soft_line() -> doc_id { return add<docs::soft_line>(); }

auto doc_manager::hard_line() -> doc_id { return add<docs::hard_line>(); }

auto doc_manager::if_break(doc_id when_broken, doc_id when_flat) -> doc_id {
    return add<docs::if_break>(when_broken, when_flat);
}

auto doc_manager::join(std::vector<doc_id> items, doc_id sep) -> doc_id {
    if (items.empty()) { return nil(); }
    std::vector<doc_id> out;
    out.reserve(items.size() * 2 - 1);
    for (usize i{0}; i < items.size(); ++i) {
        if (i != 0) { out.emplace_back(sep); }
        out.emplace_back(items[i]);
    }
    return concat(std::move(out));
}

auto doc_manager::ends_with_hard_line(doc_id id) const noexcept -> bool {
    return (*this)[id].visit(
        [&](const docs::concat& c) {
            // Trailing empties don't move the break
            for (const auto child : c.children | std::views::reverse) {
                if (child == nil()) { continue; }
                return ends_with_hard_line(child);
            }
            return false;
        },
        [&](docs::indent i) { return ends_with_hard_line(i.child); },
        [&](docs::group g) { return ends_with_hard_line(g.child); },
        [&](docs::align a) { return ends_with_hard_line(a.child); },
        [&](docs::hard_line) { return true; },
        [&](const auto&) { return false; });
}

auto doc_manager::without_trailing_hard_line(doc_id id) -> doc_id {
    // Copied out first, since adding a node may move the node storage
    const auto node{(*this)[id]};
    if (node.is<docs::hard_line>()) { return nil(); }
    if (const auto c{node.as_opt<docs::concat>()}) {
        auto children{c->children};
        for (auto& child : children | std::views::reverse) {
            if (child == nil()) { continue; }
            child = without_trailing_hard_line(child);
            break;
        }
        return concat(std::move(children));
    }
    if (const auto i{node.as_opt<docs::indent>()}) {
        return nest(without_trailing_hard_line(i->child));
    }
    if (const auto g{node.as_opt<docs::group>()}) {
        return group(without_trailing_hard_line(g->child), g->force_break);
    }
    return id;
}

auto doc_manager::contains_hard_break(doc_id id, bool nested) const noexcept -> bool {
    return (*this)[id].visit(
        [&](docs::text) { return false; },
        [&](const docs::concat& c) {
            return std::ranges::any_of(
                c.children, [&](doc_id child) { return contains_hard_break(child, nested); });
        },
        [&](docs::indent i) { return contains_hard_break(i.child, true); },
        [&](docs::group g) { return g.force_break || contains_hard_break(g.child, nested); },
        [&](docs::line_or_space) { return false; },
        [&](docs::hard_line) { return nested; },
        [&](docs::soft_line) { return false; },
        [&](docs::if_break b) { return contains_hard_break(b.when_flat, nested); },
        [&](docs::align a) { return contains_hard_break(a.child, nested); });
}

auto doc_manager::delimited(std::string_view    open,
                            std::string_view    close,
                            std::vector<doc_id> items,
                            bool                pad,
                            bool                trailing_comma,
                            bool                force_break) -> doc_id {
    return delimited(open, close, std::move(items), {}, pad, trailing_comma, force_break);
}

auto doc_manager::delimited(std::string_view    open,
                            std::string_view    close,
                            std::vector<doc_id> items,
                            std::vector<doc_id> item_trailers,
                            bool                pad,
                            bool                trailing_comma,
                            bool                force_break) -> doc_id {
    if (items.empty()) { return owned(fmt::format("{}{}", open, close)); }
    const auto edge{pad ? line() : soft_line()};

    const auto trailer_of{
        [&](usize i) -> doc_id { return i < item_trailers.size() ? item_trailers[i] : nil(); }};
    const bool has_trailers{
        std::ranges::any_of(item_trailers, [&](doc_id d) { return d != nil(); })};

    // A last item that ends its own line (a multiline string) would leave a trailing comma alone
    // on the next line, so the list's own closing break ends it instead
    const bool last_ends_line{ends_with_hard_line(items.back()) &&
                              trailer_of(items.size() - 1) == nil()};
    if (last_ends_line) { items.back() = without_trailing_hard_line(items.back()); }

    std::vector<doc_id> body;
    body.reserve(items.size() * 4);
    for (usize i{0}; i < items.size(); ++i) {
        if (i != 0) {
            body.emplace_back(text(","));
            if (trailer_of(i - 1) != nil()) { body.emplace_back(trailer_of(i - 1)); }
            body.emplace_back(line());
        }
        body.emplace_back(items[i]);
    }
    // A force-broken list always keeps its trailing comma so it round-trips as a break hint.
    if (!last_ends_line && (trailing_comma || force_break || has_trailers)) {
        body.emplace_back(if_break(text(","), nil()));
    }
    if (trailer_of(items.size() - 1) != nil()) { body.emplace_back(trailer_of(items.size() - 1)); }

    return group(concat({
                     text(open),
                     nest(concat({edge, concat(std::move(body))})),
                     edge,
                     text(close),
                 }),
                 force_break || has_trailers || last_ends_line);
}

} // namespace ghoti::syntax
