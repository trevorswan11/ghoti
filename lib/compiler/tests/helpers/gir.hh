#pragma once

#include <sstream>
#include <string>
#include <string_view>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/module.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

template <typename Dumpable> [[nodiscard]] auto dump_gir(const Dumpable& dumpable) -> std::string {
    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(dumpable);
    return ss.str();
}

template <> [[nodiscard]] auto dump_gir<ctx_idx_pair>(const ctx_idx_pair& ctx_idx) -> std::string;

// Writes the dump of `gir_mod` under GHOTI_TEST_GIR_RECORD_DIR when it is set
auto record_gir(const sema_test_context& ctx,
                const gir::module_t&     gir_mod,
                bool                     for_test_executable) -> void;

[[nodiscard]] auto dump_named_fn(helpers::sema_test_context& ctx, std::string_view name)
    -> std::string;

} // namespace ghoti::tests::helpers
