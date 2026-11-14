#pragma once

#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/type.hh"

namespace ghoti::ip {

// One per compilation: every type and every interned value, each named by its `index`
class pool {
  public:
    explicit pool(ghoti::arena& arena) noexcept : arena_{arena} {}
    ~pool() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(pool)

    [[nodiscard]] auto arena() noexcept -> ghoti::arena& { return arena_; }

    // The type `key` names, created unresolved the first time it is asked for
    [[nodiscard]] auto type_for(const sema::types::key_t& key) -> gsl::not_null<sema::type*>;
    [[nodiscard]] auto type_at(index i) const noexcept -> sema::type&;
    [[nodiscard]] auto is_type(index i) const noexcept -> bool;
    [[nodiscard]] auto size() const noexcept -> usize { return entries_.size(); }

  private:
    enum class entry_kind : u8 {
        TYPE,
    };

    struct entry {
        entry_kind                 kind;
        gsl::not_null<sema::type*> type;
    };

  private:
    [[nodiscard]] auto next_index() const noexcept -> index {
        return index{static_cast<u32>(entries_.size())};
    }

  private:
    ghoti::arena&                                           arena_;
    std::vector<entry>                                      entries_;
    ankerl::unordered_dense::map<sema::types::key_t, index> type_keys_;
};

} // namespace ghoti::ip
