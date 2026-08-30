#pragma once

#include <string>
#include <string_view>

#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

[[nodiscard]] auto dump_gir(const ctx_idx_pair& ctx_idx) -> std::string;
[[nodiscard]] auto dump_named_fn(helpers::sema_test_context& ctx, std::string_view name)
    -> std::string;

} // namespace ghoti::tests::helpers
