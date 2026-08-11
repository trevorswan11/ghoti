#pragma once

#include <string>

#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

[[nodiscard]] auto dump_gir(const ctx_idx_pair& ctx_idx) -> std::string;

} // namespace ghoti::tests::helpers
