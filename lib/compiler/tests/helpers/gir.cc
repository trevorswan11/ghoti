#include "helpers/gir.hh"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/hash.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/gir/emitter.hh"
#include "compiler/gir/module.hh"
#include "helpers/sema.hh"
#include "support/env.hh"

namespace ghoti::tests::helpers {

auto record_gir(const sema_test_context& ctx,
                const gir::module_t&     gir_mod,
                bool                     for_test_executable) -> void {
    const auto dir{get_env("GHOTI_TEST_GIR_RECORD_DIR")};
    if (!dir) { return; }

    const auto& target{ctx.analyzer.get_ctx().target_opts};
    const auto  program{fmt::format("{}|{}|{}|{}|{}",
                                    std::string_view{ctx.root_mod.source},
                                    ctx.root_path.string(),
                                    for_test_executable,
                                    target.triple_str.value_or(""),
                                    target.cpu)};
    const auto  text{gir_mod.to_string()};
    const auto  name{fmt::format("{:016x}-{:016x}.gir",
                                 stdx::hash<std::string_view>{}(program),
                                 stdx::hash<std::string_view>{}(text))};

    const std::filesystem::path path{std::filesystem::path{*dir} / name};
    if (std::filesystem::exists(path)) { return; }
    std::ofstream out{path, std::ios::binary};
    fmt::print(out, "{}", text);
}

template <> [[nodiscard]] auto dump_gir<ctx_idx_pair>(const ctx_idx_pair& ctx_idx) -> std::string {
    gir::emitter emitter{ctx_idx.first->analyzer.get_ctx(), ctx_idx.first->root_mod};
    const auto   gir_mod{emitter.emit()};
    record_gir(*ctx_idx.first, gir_mod, false);

    std::ostringstream ss;
    gir::dumper        dumper{ss};
    dumper.dump(gir_mod);
    return ss.str();
}

auto dump_named_fn(helpers::sema_test_context& ctx, std::string_view name) -> std::string {
    gir::emitter emitter{ctx.analyzer.get_ctx(), ctx.root_mod};
    const auto   gir_mod{emitter.emit()};
    record_gir(ctx, gir_mod, false);

    for (const auto fn : gir_mod.get_functions()) {
        if (fn->get_name() != name) { continue; }
        std::ostringstream ss;
        gir::dumper{ss}.dump(*fn);
        return std::string{ss.view()};
    }
    FAIL("function not found in GIR module");
    return {};
}

} // namespace ghoti::tests::helpers
