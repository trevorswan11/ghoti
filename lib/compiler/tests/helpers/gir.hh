#pragma once

#include <sstream>
#include <string>
#include <string_view>

#include "compiler/gir/dumper.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

template <typename Dumpable> [[nodiscard]] auto dump_gir(const Dumpable& dumpable) -> std::string {
    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(dumpable);
    return ss.str();
}

template <> [[nodiscard]] auto dump_gir<ctx_idx_pair>(const ctx_idx_pair& ctx_idx) -> std::string;

[[nodiscard]] auto dump_named_fn(helpers::sema_test_context& ctx, std::string_view name)
    -> std::string;

} // namespace ghoti::tests::helpers
