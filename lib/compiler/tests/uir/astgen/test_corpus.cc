#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/option.hh>

#include "compiler/codegen/target.hh"
#include "compiler/module/file_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/stdlib.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/context.hh"
#include "compiler/uir/astgen.hh"
#include "support/path_utils.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// The shipped sources, loaded from disk with `std` registered as the driver registers it
class corpus_context {
  public:
    explicit corpus_context(bool testing)
        : cfg_{.target_opts    = {},
               .facts          = codegen::target_facts::resolve(stdx::none),
               .build_mode     = sema::optimize_mode::DEBUG,
               .runtime_safety = true,
               .testing        = testing,
               .modules        = manager_} {
        REQUIRE(manager_.add_library_module("std", UNWRAP(mod::find_stdlib())));
    }

    // Lowers `root` and everything it reaches, requiring every module to lower cleanly
    auto check_lowers(const mod::module_t& root) -> void {
        const auto lowered{uir::lower_reachable(root, {.cfg = cfg_, .modules = manager_})};
        CHECK_FALSE(lowered.empty());
        for (const auto& item : lowered) {
            if (item.result.diagnostics.empty()) { continue; }
            FAIL_CHECK(fmt::format("{} lowered with errors:\n{}",
                                   path_utils::display(item.source->path),
                                   item.result.diagnostics));
        }
    }

    auto check_file_lowers(const std::filesystem::path& path) -> void {
        check_lowers(*UNWRAP(manager_.try_get_file_module(path)));
    }

    [[nodiscard]] auto manager() noexcept -> mod::module_manager& { return manager_; }

  private:
    mod::file_loader    loader_;
    mod::module_manager manager_{loader_};
    sema::cfg_context   cfg_;
};

[[nodiscard]] auto repo_root() -> std::filesystem::path {
    return UNWRAP(mod::find_stdlib()).parent_path().parent_path().parent_path();
}

} // namespace

TEST_CASE("The standard library lowers, for a build and for a test") {
    const auto     testing{GENERATE(false, true)};
    corpus_context ctx{testing};
    ctx.check_file_lowers(UNWRAP(mod::find_stdlib()));
}

// The old pipeline needed a resolve-order hack for this (sema/test_analyzer.cc); AstGen lowers
// each module on its own, so a std module is a root like any other
TEST_CASE("A std module lowers as the root of a test build") {
    corpus_context ctx{true};
    ctx.check_file_lowers(UNWRAP(mod::find_stdlib()).parent_path() / "fmt" / "fmt.gh");
}

TEST_CASE("The program entry lowers") {
    corpus_context ctx{false};
    ctx.check_file_lowers(UNWRAP(mod::find_start()));
}

TEST_CASE("compiler_rt lowers") {
    corpus_context ctx{false};
    ctx.check_file_lowers(UNWRAP(mod::find_lib_path("compiler_rt/compiler_rt.gh")));
}

TEST_CASE("The builtin prelude lowers") {
    corpus_context ctx{false};
    ctx.check_lowers(ctx.manager().get_or_create_builtin_module(sema::builtin_module_source({})));
}

TEST_CASE("Every example lowers") {
    std::vector<std::filesystem::path> examples;
    for (const auto& entry : std::filesystem::directory_iterator{repo_root() / "examples"}) {
        if (entry.path().extension() == ".gh") { examples.emplace_back(entry.path()); }
    }
    REQUIRE_FALSE(examples.empty());
    for (const auto& example : examples) {
        corpus_context ctx{false};
        ctx.check_file_lowers(example);
    }
}

} // namespace ghoti::tests
