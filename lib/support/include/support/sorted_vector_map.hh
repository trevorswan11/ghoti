#pragma once

#include <algorithm>
#include <concepts>
#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti {

// A map kept as one key-sorted vector: logarithmic lookup and an O(1) append for ascending keys
template <std::totally_ordered Key, typename Value> class sorted_vector_map {
  public:
    using value_type     = std::pair<Key, Value>;
    using container_type = std::vector<value_type>;
    using iterator       = typename container_type::iterator;
    using const_iterator = typename container_type::const_iterator;

  public:
    // Replaces the value of an existing key, so the last write for a key wins
    auto insert_or_assign(Key key, Value value) -> Value& {
        if (entries_.empty() || entries_.back().first < key) {
            return entries_.emplace_back(std::move(key), std::move(value)).second;
        }

        const auto it{lower_bound(key)};
        if (it != entries_.end() && it->first == key) {
            it->second = std::move(value);
            return it->second;
        }
        return entries_.emplace(it, std::move(key), std::move(value))->second;
    }

    [[nodiscard]] auto find(this auto&& self, const Key& key) noexcept {
        using result_t = stdx::option<decltype((self.entries_.front().second))>;
        const auto it{self.lower_bound(key)};
        if (it == self.entries_.end() || it->first != key) { return result_t{}; }
        return result_t{it->second};
    }

    [[nodiscard]] auto contains(const Key& key) const noexcept -> bool {
        return find(key).has_value();
    }

    auto reserve(usize capacity) -> void { entries_.reserve(capacity); }

    [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }
    [[nodiscard]] auto size() const noexcept -> usize { return entries_.size(); }

    [[nodiscard]] auto begin(this auto&& self) noexcept { return self.entries_.begin(); }
    [[nodiscard]] auto end(this auto&& self) noexcept { return self.entries_.end(); }

  private:
    [[nodiscard]] auto lower_bound(this auto&& self, const Key& key) noexcept {
        return std::ranges::lower_bound(self.entries_, key, {}, &value_type::first);
    }

  private:
    container_type entries_;
};

} // namespace ghoti
