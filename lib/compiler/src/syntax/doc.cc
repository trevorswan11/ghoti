#include "compiler/syntax/doc.hh"

#include <cstring>
#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/types.hh>

namespace ghoti::syntax {

auto doc_manager::nil() -> doc_id { return text(std::string_view{""}); }

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

auto doc_manager::delimited(std::string_view    open,
                            std::string_view    close,
                            std::vector<doc_id> items,
                            bool                pad,
                            bool                trailing_comma) -> doc_id {
    if (items.empty()) { return owned(fmt::format("{}{}", open, close)); }
    const auto edge{pad ? line() : soft_line()};

    std::vector<doc_id> body;
    body.reserve(items.size() * 3 - 1);
    for (usize i{0}; i < items.size(); ++i) {
        if (i != 0) {
            body.emplace_back(text(","));
            body.emplace_back(line());
        }
        body.emplace_back(items[i]);
    }
    if (trailing_comma) { body.emplace_back(if_break(text(","), nil())); }

    return group(concat({
        text(open),
        nest(concat({edge, concat(std::move(body))})),
        edge,
        text(close),
    }));
}

} // namespace ghoti::syntax
