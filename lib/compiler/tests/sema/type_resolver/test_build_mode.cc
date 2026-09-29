#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

// Resolves `source` as a build in `mode`, with runtime safety as that mode implies
[[nodiscard]] auto resolve_in_mode(std::string_view source, sema::optimize_mode mode)
    -> stdx::box<helpers::sema_test_context> {
    auto ctx{stdx::make_box<helpers::sema_test_context>(
        std::vector<helpers::mock_file>{}, helpers::TEST_FILENAME, source)};
    helpers::check_errors<syntax::diagnostics>(ctx->root_mod);
    ctx->analyzer.set_optimize_mode(mode);
    ctx->analyzer.set_runtime_safety(mode == sema::optimize_mode::DEBUG ||
                                     mode == sema::optimize_mode::RELEASE_SAFE);
    ctx->analyzer.collect_symbols(ctx->root_mod);
    ctx->analyzer.resolve_types(ctx->root_mod);
    return ctx;
}

auto check_clean_in_mode(std::string_view source, sema::optimize_mode mode) -> void {
    const auto ctx{resolve_in_mode(source, mode)};
    helpers::check_errors<sema::diagnostics>(ctx->root_mod);
}

} // namespace

TEST_CASE("@optimizeMode() and @runtimeSafety() fold to the build's mode") {
    check_clean_in_mode(R"(
        constexpr {
            @assert(@optimizeMode() == .debug);
            @assert(@optimizeMode() == builtin.OptimizeMode.debug);
            @assert(@runtimeSafety());
        }
    )",
                        sema::optimize_mode::DEBUG);
    check_clean_in_mode(R"(
        constexpr {
            @assert(@optimizeMode() == .release_fast);
            @assert(!@runtimeSafety());
        }
    )",
                        sema::optimize_mode::RELEASE_FAST);
    check_clean_in_mode(R"(
        constexpr {
            @assert(@optimizeMode() == .release_safe);
            @assert(@runtimeSafety());
        }
    )",
                        sema::optimize_mode::RELEASE_SAFE);
}

TEST_CASE("`optimize` and `safety` are @cfg names") {
    constexpr std::string_view source{R"(
        @cfg (optimize == .debug) {
            const level := 0;
        } else @cfg (optimize == .release_small) {
            const level := 2;
        } else {
            const level := 1;
        }
        @cfg (safety) {
            const checked := true;
        } else {
            const checked := false;
        }
        const tag := @cfgValue(safety);
    )"};
    check_clean_in_mode(
        fmt::format("{}\nconstexpr {{ @assert(level == 0 and checked and tag); }}", source),
        sema::optimize_mode::DEBUG);
    check_clean_in_mode(
        fmt::format("{}\nconstexpr {{ @assert(level == 2 and !checked and !tag); }}", source),
        sema::optimize_mode::RELEASE_SMALL);
    check_clean_in_mode(fmt::format("{}\nconstexpr {{ @assert(level == 1 and checked); }}", source),
                        sema::optimize_mode::RELEASE_SAFE);
}

TEST_CASE("A misspelled build mode in @cfg is an error") {
    const auto ctx{resolve_in_mode("@cfg (optimize == .relase_fast) { const x := 1; }",
                                   sema::optimize_mode::DEBUG)};
    CHECK_FALSE(ctx->root_mod.is_ok());
}

TEST_CASE("An attribute argument can depend on the build mode") {
    check_clean_in_mode(R"(
        @[inline(if (@optimizeMode() == .debug) .never else .always)]
        const f := fn(x: i32): i32 { return x; };
        pub const main := fn(): i32 { return f(0); };
    )",
                        sema::optimize_mode::RELEASE_FAST);
}

} // namespace ghoti::tests
