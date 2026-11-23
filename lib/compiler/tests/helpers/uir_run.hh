#pragma once

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/sema/sema.hh"
#include "helpers/uir_diff.hh"
#include "support/int128.hh"
#include "support/test.hh"

namespace ghoti::tests::helpers {

// A UIR Sema run of one program, read back by declaration name
class uir_run {
  public:
    explicit uir_run(std::string_view source, const pipeline_settings& settings = {})
        : run_{run_uir_sema(source, settings)} {}

    [[nodiscard]] auto out() const -> const uir::sema::outcome& { return *run_->out; }

    [[nodiscard]] auto messages() const -> std::vector<std::string> {
        std::vector<std::string> found;
        for (const auto& d : out().diagnostics) {
            found.emplace_back(d.get_message().value_or(std::string{}));
        }
        return found;
    }

    [[nodiscard]] auto codes() const -> std::vector<sema::error> {
        std::vector<sema::error> found;
        for (const auto& d : out().diagnostics) { found.emplace_back(d.get_error()); }
        return found;
    }

    [[nodiscard]] auto int_value(std::string_view name) const -> i128 {
        const auto& v{pool().value_at(root_index(name))};
        const auto  exact{
            v.data.visit([](i64 x) -> stdx::option<i128> { return i128{x}; },
                         [](u64 x) -> stdx::option<i128> { return static_cast<i128>(x); },
                         [](const auto&) -> stdx::option<i128> { return stdx::none; })};
        return UNWRAP(exact);
    }

    [[nodiscard]] auto type_name(std::string_view name) const -> std::string {
        const auto index{root_index(name)};
        REQUIRE(pool().is_type(index));
        return run_->analyzer->get_ctx().type_display_name(pool().type_at(index));
    }

    [[nodiscard]] auto gir() const -> std::string {
        REQUIRE(out().succeeded());
        std::ostringstream text;
        gir::dumper{text}.dump(*out().gir);
        return text.str();
    }

    [[nodiscard]] auto pool() const -> ip::pool& { return run_->analyzer->get_ctx().pool.intern(); }

  private:
    [[nodiscard]] auto root_index(std::string_view name) const -> ip::index {
        const auto found{out().root_values.find(std::string{name})};
        if (found == out().root_values.end()) { FAIL(fmt::format("'{}' has no value", name)); }
        return found->second;
    }

  private:
    stdx::box<uir_sema_run> run_;
};

[[nodiscard]] inline auto uir_accepted(std::string_view source) -> uir_run {
    uir_run run{source};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    if (!run.out().succeeded()) {
        FAIL("UIR Sema reported:\n" << fmt::format("{}", run.out().diagnostics));
    }
    return run;
}

[[nodiscard]] inline auto uir_rejected(std::string_view source) -> uir_run {
    uir_run run{source};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    REQUIRE_FALSE(run.out().diagnostics.empty());
    return run;
}

} // namespace ghoti::tests::helpers
