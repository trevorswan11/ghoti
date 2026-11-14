#include "compiler/ip/pool.hh"

#include <utility>

#include <gsl/pointers>
#include <stdx/assert.hh>

#include "compiler/sema/type.hh"

namespace ghoti::ip {

auto pool::type_for(const sema::types::key_t& key) -> gsl::not_null<sema::type*> {
    if (const auto it{type_keys_.find(key)}; it != type_keys_.end()) {
        return &type_at(it->second);
    }
    const gsl::not_null<sema::type*> created{arena_.make<sema::type>(key).get()};
    const auto                       at{next_index()};
    created->ip_index_ = at;
    entries_.emplace_back(entry_kind::TYPE, created);
    type_keys_.emplace(key, at);
    return created;
}

auto pool::type_at(index i) const noexcept -> sema::type& {
    const auto& e{entries_[std::to_underlying(i)]};
    ASSERT(e.kind == entry_kind::TYPE, "The intern pool entry is not a type");
    return *e.type;
}

auto pool::is_type(index i) const noexcept -> bool {
    return entries_[std::to_underlying(i)].kind == entry_kind::TYPE;
}

} // namespace ghoti::ip
