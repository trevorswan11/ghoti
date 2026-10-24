#pragma once

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::sema {

// Tells apart the instances of one type constructor
enum class instance_id : u64 {};

// An instance id from a hash, which is never the none value
[[nodiscard]] constexpr auto make_instance_id(u64 hash) noexcept -> instance_id {
    return instance_id{hash | 1};
}

} // namespace ghoti::sema

template <> struct stdx::nullable<ghoti::sema::instance_id> {
    [[nodiscard]] static constexpr auto invalid() noexcept -> ghoti::sema::instance_id {
        return ghoti::sema::instance_id{0};
    }
    [[nodiscard]] static constexpr auto is_valid(const ghoti::sema::instance_id& id) noexcept
        -> bool {
        return id != invalid();
    }
};
