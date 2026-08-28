#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "compiler/gir/emitter.hh"
#include "compiler/gir/module.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view WIN_MODULE{R"(
    extern("kernel32") const GetLastError: fn(): u32;
    pub const last_error := fn(): u32 { return GetLastError(); };
)"};

[[nodiscard]] auto emit(helpers::sema_test_context& ctx) -> gir::module {
    gir::emitter emitter{ctx.analyzer.get_ctx(), ctx.root_mod};
    return emitter.emit();
}

} // namespace

TEST_CASE("GIR reachability: unreferenced imported extern contributes no library") {
    auto [ctx, idx]{helpers::resolve_and_check(
        R"(
            import "win.gh" as win;
            pub const main := fn(args: [][:0]u8): void {};
        )",
        {helpers::mock_file{"win.gh", WIN_MODULE, "win"}})};

    auto gir_mod{emit(*ctx)};
    CHECK(gir_mod.has_function("GetLastError"));
    CHECK(gir_mod.has_function("last_error"));
    REQUIRE(gir_mod.get_required_libraries().size() == 1);

    const std::vector<std::string_view> roots{"main"};
    gir_mod.prune_unreachable(roots);

    CHECK_FALSE(gir_mod.has_function("GetLastError"));
    CHECK_FALSE(gir_mod.has_function("last_error"));
    CHECK(gir_mod.get_required_libraries().empty());
}

TEST_CASE("GIR reachability: transitively referenced imported extern is retained") {
    auto [ctx, idx]{helpers::resolve_and_check(
        R"(
            import "win.gh" as win;
            pub const main := fn(args: [][:0]u8): void {
                const e := win::last_error();
            };
        )",
        {helpers::mock_file{"win.gh", WIN_MODULE, "win"}})};

    auto gir_mod{emit(*ctx)};

    const std::vector<std::string_view> roots{"main"};
    gir_mod.prune_unreachable(roots);

    CHECK(gir_mod.has_function("GetLastError"));
    CHECK(gir_mod.has_function("last_error"));
    REQUIRE(gir_mod.get_required_libraries().size() == 1);
    CHECK(gir_mod.get_required_libraries()[0] == "kernel32");
}

TEST_CASE("GIR reachability: root-module externs are never pruned") {
    auto [ctx, idx]{helpers::resolve_and_check(R"(
        extern("kernel32") const GetLastError: fn(): u32;
        pub const main := fn(args: [][:0]u8): void {};
    )")};

    auto gir_mod{emit(*ctx)};

    const std::vector<std::string_view> roots{"main"};
    gir_mod.prune_unreachable(roots);

    CHECK(gir_mod.has_function("GetLastError"));
    REQUIRE(gir_mod.get_required_libraries().size() == 1);
}

} // namespace ghoti::tests
