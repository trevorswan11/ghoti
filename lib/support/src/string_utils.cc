#include "support/string_utils.hh"

#include <string>
#include <string_view>

namespace ghoti::string_utils {

auto to_u8string(std::string_view s) -> std::u8string {
    std::u8string out;
    out.reserve(s.size());
    for (const auto c : s) { out.push_back(static_cast<char8_t>(c)); }
    return out;
}

auto to_utf8(const std::u8string& s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (const auto c : s) { out.push_back(static_cast<char>(c)); }
    return out;
}

} // namespace ghoti::string_utils
