#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/arena.hh>
#include <stdx/fixed/string.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/arena.hh"

namespace ghoti::syntax {

// Deduplicating store for ident names and string-literal values
class string_interner {
  public:
    explicit string_interner(ghoti::arena& arena) noexcept : arena_{arena} {}

    [[nodiscard]] auto intern(std::string_view text) -> std::string_view {
        if (text.empty()) { return {}; }
        if (const auto it{index_.find(text)}; it != index_.end()) { return it->second; }

        std::string_view stored{};
        if (text.size() <= MAX_ARENA_BYTES) {
            const auto span{arena_->make_span<char>(text.size())};
            std::memcpy(span.data(), text.data(), text.size());
            stored = std::string_view{span.data(), span.size()};
        } else {
            stored = overflow_.emplace_back(text);
        }

        index_.emplace(stored, stored);
        return stored;
    }

    // Drops the dedup table and any overflow storage
    auto clear() noexcept -> void {
        index_.clear();
        overflow_.clear();
    }

  private:
    // Leave room for the arena's own bookkeeping within a block.
    static constexpr usize MAX_ARENA_BYTES{stdx::DEFAULT_ARENA_BLOCK_SIZE / 2};

  private:
    stdx::option<ghoti::arena&>                                      arena_;
    ankerl::unordered_dense::map<std::string_view, std::string_view> index_;
    std::vector<stdx::fixed::string>                                 overflow_;
};

} // namespace ghoti::syntax
