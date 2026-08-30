#include "helpers/gir.hh"

#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

auto dump_gir(const ctx_idx_pair& ctx_idx) -> std::string {
    gir::emitter emitter{ctx_idx.first->analyzer.get_ctx(), ctx_idx.first->root_mod};
    const auto   gir_mod{emitter.emit()};

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    return ss.str();
}

auto dump_named_fn(helpers::sema_test_context& ctx, std::string_view name) -> std::string {
    gir::emitter emitter{ctx.analyzer.get_ctx(), ctx.root_mod};
    const auto   gir_mod{emitter.emit()};

    for (const auto* fn : gir_mod.get_functions()) {
        if (fn->get_name() != name) { continue; }
        std::ostringstream ss;
        gir::dumper{ss}.dump(*fn);
        return std::string{ss.view()};
    }
    FAIL("function not found in GIR module");
    return {};
}

} // namespace ghoti::tests::helpers
