#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/module.hh"
#include "compiler/module/parse_cache.hh"
#include "helpers/sema.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

// Gated block statements and struct fields, whose losing arms would not type check
constexpr std::string_view DEP{R"(
    pub const S = struct {
        @cfg(ptr_bits == 7) { unused: NoSuchType }
        else                { pub fallback: i32 }
    };
    pub const f = fn(): i32 {
        @cfg(ptr_bits == 7) { const bad: i32 = true; }
        return 40;
    };
)"};

constexpr std::string_view ROOT{R"(
    import "dep.gh" as dep;
    pub const g = fn(): i32 {
        let s = dep.S{ .fallback = 2 };
        return s.fallback + dep.f();
    };
)"};

// Analyzes `ROOT` in a fresh manager that borrows `dep.gh` from `cache`
auto analyze_with(mod::parse_cache& cache) -> stdx::box<helpers::sema_test_context> {
    auto ctx{stdx::make_box<helpers::sema_test_context>(
        std::vector<helpers::mock_file>{{"dep.gh", DEP}}, "root.gh", ROOT, std::cerr)};
    ctx->manager.set_parse_cache(cache);
    REQUIRE(ctx->analyzer.analyze("root.gh"));
    CHECK_FALSE(ctx->manager.any_errored());
    return ctx;
}

[[nodiscard]] auto dep_of(helpers::sema_test_context& ctx) -> const mod::module_t& {
    return *UNWRAP(ctx.manager.try_get_file_module("dep.gh"));
}

} // namespace

TEST_CASE("Analyses borrowing one cached parse each apply its @cfg gates") {
    mod::parse_cache cache;
    auto             first{analyze_with(cache)};
    auto             second{analyze_with(cache)};

    CHECK(cache.size() == 1);
    REQUIRE(dep_of(*first).parsed_from);
    CHECK(dep_of(*first).parsed_from == dep_of(*second).parsed_from);
}

TEST_CASE("A parse cache reparses a file whose text changed") {
    mod::parse_cache cache;
    const auto       before{cache.get("a.gh", "const x = 1;")};
    CHECK(cache.get("a.gh", "const x = 1;") == before);

    const auto after{cache.get("a.gh", "const x = 2;")};
    CHECK(after != before);
    CHECK(cache.size() == 1);

    cache.retain([](const std::filesystem::path&) { return false; });
    CHECK(cache.size() == 0);
}

} // namespace ghoti::tests
