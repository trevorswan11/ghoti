#include "helpers/uir.hh"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/memory.hh>

#include "compiler/codegen/target.hh"
#include "compiler/sema/context.hh"
#include "compiler/uir/dumper.hh"
#include "helpers/common.hh"
#include "support/test.hh"

namespace ghoti::tests::helpers {

namespace {

[[nodiscard]] auto sorted_keys(gsl::span<const sema::diagnostic> diags)
    -> std::vector<std::string> {
    std::vector<std::string> keys;
    for (const auto& d : diags) { keys.push_back(d.identity()); }
    std::ranges::sort(keys);
    return keys;
}

} // namespace

uir_test_context::uir_test_context(const std::vector<mock_file>& imports,
                                   const std::filesystem::path&  root_path,
                                   std::string_view              input,
                                   bool                          testing)
    : loader{stdx::make_box<mod::memory_loader>()}, manager{*loader},
      cfg{.target_opts    = {},
          .facts          = codegen::target_facts::resolve(stdx::none),
          .build_mode     = sema::optimize_mode::DEBUG,
          .runtime_safety = true,
          .testing        = testing,
          .modules        = manager},
      root{[&] -> auto& {
          loader->add(root_path, std::string{input});
          for (const auto& mock : imports) {
              loader->add(mock.path, std::string{mock.source});
              if (mock.name) { REQUIRE(manager.add_library_module(*mock.name, mock.path)); }
          }
          return *UNWRAP(manager.try_get_file_module(root_path));
      }()} {}

auto uir_test_context::lower(const mod::module_t& mod) -> uir::lowered {
    return uir::lower_module(mod, {.cfg = cfg, .modules = manager});
}

auto lower_uir(std::string_view input, const std::vector<mock_file>& imports, bool testing)
    -> uir::lowered {
    uir_test_context ctx{imports, TEST_FILENAME, input, testing};
    check_errors<syntax::diagnostics>(ctx.root);
    return ctx.lower(ctx.root);
}

auto dump_uir(std::string_view input, const std::vector<mock_file>& imports) -> std::string {
    auto lowered{lower_uir(input, imports)};
    if (!lowered.diagnostics.empty()) { fmt::println("{}", lowered.diagnostics); }
    CHECK(lowered.diagnostics.empty());
    return uir::dump(lowered.uir);
}

auto check_astgen_against(std::string_view                  input,
                          const std::vector<mock_file>&     imports,
                          gsl::span<const sema::diagnostic> expected) -> void {
    // The collector always walked test bodies, which AstGen lowers only for a test build
    const auto lowered{lower_uir(input, imports, true)};
    const auto actual{sorted_keys(lowered.diagnostics)};
    const auto wanted{sorted_keys(expected)};
    if (actual != wanted) {
        fmt::println("AstGen reported:\n{}", lowered.diagnostics);
        fmt::println("expected:\n{}", fmt::join(wanted, "\n"));
    }
    CHECK(actual == wanted);
}

auto check_lowers_through_astgen(sema_test_context& ctx) -> void {
    using mod::module_state;
    const auto state{ctx.root_mod.state};
    const bool accepted{
        (state == module_state::TYPE_RESOLVED || state == module_state::TYPE_CHECKED) &&
        !ctx.manager.any_errored()};
    if (!accepted) { return; }

    mod::module_manager fresh{*ctx.loader};
    for (const auto& [name, path] : ctx.libraries) {
        REQUIRE(fresh.add_library_module(name, path));
    }
    const auto&             sema_ctx{ctx.analyzer.get_ctx()};
    const sema::cfg_context cfg{.target_opts = sema_ctx.target_opts,
                                .facts =
                                    codegen::target_facts::resolve(sema_ctx.target_opts.triple_str),
                                .build_mode     = sema_ctx.build_mode,
                                .runtime_safety = sema_ctx.runtime_safety,
                                .testing        = sema_ctx.testing,
                                .modules        = fresh};
    const auto&             root{*UNWRAP(fresh.try_get_file_module(ctx.root_path))};
    for (const auto& item : uir::lower_reachable(root, {.cfg = cfg, .modules = fresh})) {
        if (item.result.diagnostics.empty()) { continue; }
        FAIL_CHECK(fmt::format("the old pipeline accepted {} but AstGen reported:\n{}",
                               item.source->path.generic_string(),
                               item.result.diagnostics));
    }
}

} // namespace ghoti::tests::helpers
