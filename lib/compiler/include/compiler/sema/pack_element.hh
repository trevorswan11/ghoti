#pragma once

#include <charconv>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

namespace ghoti::sema {

// Element `index` of the parameter pack `pack`, bound and captured by the hidden name `pack#index`
struct pack_element {
    std::string_view pack;
    usize            index;

    [[nodiscard]] static auto parse(std::string_view name) -> stdx::option<pack_element> {
        const auto hash{name.find('#')};
        if (hash == std::string_view::npos) { return stdx::none; }
        const auto digits{stdx::string::substr(name, hash + 1)};
        usize      index{0};
        const auto [end, ec]{std::from_chars(digits.data(), digits.data() + digits.size(), index)};
        if (ec != std::errc{} || end != digits.data() + digits.size()) { return stdx::none; }
        return pack_element{.pack = stdx::string::substr(name, 0, hash), .index = index};
    }

    [[nodiscard]] auto hidden_name() const -> std::string {
        return fmt::format("{}#{}", pack, index);
    }
    // How the source spells it
    [[nodiscard]] auto spelled() const -> std::string { return fmt::format("{}[{}]", pack, index); }
};

} // namespace ghoti::sema
