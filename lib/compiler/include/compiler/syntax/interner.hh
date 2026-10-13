#pragma once

#include <cstring>
#include <string_view>

#include <ankerl/unordered_dense.h>
#include <stdx/arena.hh>
#include <stdx/assert.hh>
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

        const auto span{arena_->make_span<char>(text.size())};
        std::memcpy(span.data(), text.data(), text.size());
        std::string_view stored{span.data(), span.size()};

        index_.emplace(stored, stored);
        return stored;
    }

    // Asserts that the arena is actually valid
    [[nodiscard]] auto arena() noexcept -> ghoti::arena& {
        VERIFY(arena_, "Arena was not set and cannot back the AST");
        return *arena_;
    }

    // Drops the dedup table
    auto clear() noexcept -> void { index_.clear(); }

  private:
    stdx::option<ghoti::arena&>                                      arena_;
    ankerl::unordered_dense::map<std::string_view, std::string_view> index_;
};

} // namespace ghoti::syntax
