#include <atomic>
#include <sstream>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/utility.hh>

#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view SOURCE{R"(
    const ok: i32 = 1;
    const bad: i32 = true;
    test { const also_bad: i32 = true; }
)"};

} // namespace

TEST_CASE("An analysis cancelled before it starts resolves no declarations") {
    std::ostringstream         ss;
    helpers::sema_test_context ctx{{}, "root.gh", SOURCE, ss};
    const std::atomic<bool>    cancelled{true};
    ctx.analyzer.set_testing(true);
    ctx.analyzer.set_cancellation(cancelled);

    DISCARD(ctx.analyzer.analyze("root.gh"));
    CHECK_FALSE(ctx.root_mod.is_poisoned());
    CHECK(ss.view().empty());
}

TEST_CASE("An analysis that is never cancelled reports what the cancelled one skipped") {
    std::ostringstream         ss;
    helpers::sema_test_context ctx{{}, "root.gh", SOURCE, ss};
    const std::atomic<bool>    cancelled{false};
    ctx.analyzer.set_testing(true);
    ctx.analyzer.set_cancellation(cancelled);

    DISCARD(ctx.analyzer.analyze("root.gh"));
    CHECK(ctx.root_mod.is_poisoned());
    CHECK(ss.view().contains("bad"));
}

} // namespace ghoti::tests
