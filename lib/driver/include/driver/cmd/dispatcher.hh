#pragma once

#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

namespace ghoti::cmd {

class debug;

class dispatcher {
  public:
    static auto operator()(debug& dump) -> stdx::result<void, i32>;
    static auto operator()(stdx::monostate) noexcept -> stdx::result<void, i32> { return {}; }
};

} // namespace ghoti::cmd
