#include "helpers/gir.hh"

#include <sstream>
#include <string>

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

} // namespace ghoti::tests::helpers
