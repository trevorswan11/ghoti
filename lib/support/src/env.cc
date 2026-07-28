#include "support/env.hh"

#include <cstdlib>
#include <string>
#include <string_view>

#include <stdx/option.hh>

#include "ghoti/config.h"

#if GHOTI_WINDOWS
#    include <sec_api/stdlib_s.h>
#else
#    include <stdlib.h>
#endif

namespace ghoti {

auto set_env(const std::string& name, const std::string& value) -> void {
#if GHOTI_WINDOWS
    ::_putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

auto get_env(const std::string& name) -> stdx::option<std::string_view> {
    const auto* var{std::getenv(name.c_str())};
    if (!var) { return stdx::none; }
    return var;
}

} // namespace ghoti
