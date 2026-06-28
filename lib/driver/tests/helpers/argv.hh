#pragma once

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/iterator.hh>
#include <stdx/string.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>

namespace ghoti::tests::helpers {

class mock_argv {
  public:
    MAKE_ITERATOR(args_t, std::vector<char*>, pointers_)

  public:
    template <stdx::StringLike... Strings> explicit mock_argv(Strings&&... args) {
        strings_.reserve(sizeof...(Strings));
        (..., strings_.emplace_back(args));

        REQUIRE(strings_.size() >= 1);
        pointers_.reserve(strings_.size() + 1);
        for (auto& s : strings_) { pointers_.emplace_back(s.data()); }
        pointers_.emplace_back(nullptr);
    }

    [[nodiscard]] auto argv() const noexcept -> char** { return pointers_.data(); }
    template <stdx::NumericIntegral I = i32> [[nodiscard]] auto argc() const noexcept -> I {
        return static_cast<I>(strings_.size());
    }

    [[nodiscard]] auto operator[](usize idx) const noexcept -> char* {
        REQUIRE(idx < pointers_.size());
        return pointers_[idx];
    }

  private:
    std::vector<std::string> strings_;
    mutable args_t           pointers_;
};

} // namespace ghoti::tests::helpers
