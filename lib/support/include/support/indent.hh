#pragma once

#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace ghoti {

namespace symbols {

constexpr std::string_view VERT_BAR{"\u2502   "};
constexpr std::string_view T_BRANCH{"\u251C\u2500 "};
constexpr std::string_view L_BRANCH{"\u2514\u2500 "};
constexpr std::string_view EMPTY{"    "};

} // namespace symbols

class indent {
  public:
    class guard {
      public:
        constexpr guard(indent& i, bool last) : indent_{i} { indent_.push(last); }
        ~guard() { indent_.pop(); }

      private:
        indent& indent_;
    };

  public:
    constexpr auto push(bool last) -> void { levels_.push_back(last); }
    constexpr auto pop() -> void { levels_.pop_back(); }

    [[nodiscard]] constexpr auto current_branch() const -> std::string {
        if (levels_.empty()) { return {}; }

        // The people do not speak of this...
        auto res{levels_ | std::views::take(levels_.size() - 1) |
                 std::views::transform([](auto level) -> std::string_view {
                     return level ? symbols::EMPTY : symbols::VERT_BAR;
                 }) |
                 std::views::join | std::ranges::to<std::string>()};
        res += levels_.back() ? symbols::L_BRANCH : symbols::T_BRANCH;
        return res;
    }

  private:
    std::vector<bool> levels_;
};

} // namespace ghoti
