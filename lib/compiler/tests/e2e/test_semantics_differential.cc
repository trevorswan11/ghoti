#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "helpers/differential.hh"
#include "support/env.hh"

namespace ghoti::tests {

namespace {

namespace diff = differential;

auto expect_agreement(const std::vector<diff::expr_template>& templates,
                      const diff::options&                    opts = {}) -> void {
    const auto               reports{diff::check_all(templates, opts)};
    std::vector<std::string> waiting;
    for (usize i{0}; i < templates.size(); ++i) {
        const auto& rep{reports[i]};
        if (!rep.waiting_on.empty()) {
            waiting.emplace_back(fmt::format("{} -> {} ({})",
                                             templates[i].text,
                                             templates[i].result.name,
                                             fmt::join(rep.waiting_on, ", ")));
        }
        if (rep.mismatches.empty() && rep.not_foldable.empty()) { continue; }
        INFO(diff::describe(templates[i], rep));
        CHECK(rep.mismatches.empty());
        CHECK(rep.not_foldable.empty());
    }
    // Folding is still checked for these; running them waits on lib/compiler_rt
    if (!waiting.empty() && !opts.random_only) {
        WARN(fmt::format("runtime half needs compiler_rt:\n  {}", fmt::join(waiting, "\n  ")));
    }
}

auto binary(std::string_view op, const diff::scalar_type& type, std::string unchecked = {})
    -> diff::expr_template {
    return {
        .text                 = fmt::format("{{0}} {} {{1}}", op),
        .operands             = {type, type},
        .result               = type,
        .unchecked_equivalent = std::move(unchecked),
    };
}

auto comparison(std::string_view op, const diff::scalar_type& type) -> diff::expr_template {
    return {
        .text     = fmt::format("{{0}} {} {{1}}", op),
        .operands = {type, type},
        .result   = diff::bool_type(),
    };
}

auto unary(std::string_view op, const diff::scalar_type& type, std::string unchecked = {})
    -> diff::expr_template {
    return {
        .text                 = fmt::format("{}{{0}}", op),
        .operands             = {type},
        .result               = type,
        .unchecked_equivalent = std::move(unchecked),
    };
}

auto call(std::string_view builtin, const diff::scalar_type& type, usize arity)
    -> diff::expr_template {
    std::vector<std::string> args;
    for (usize i{0}; i < arity; ++i) { args.emplace_back(fmt::format("{{{}}}", i)); }
    return {
        .text     = fmt::format("{}({})", builtin, fmt::join(args, ", ")),
        .operands = std::vector<diff::scalar_type>(arity, type),
        .result   = type,
    };
}

// Families whose operations never call compiler_rt cover the wide types by default too
auto int_types_with_wide() -> std::vector<diff::scalar_type> {
    auto types{diff::int_types()};
    // `GHOTI_DIFF_FULL` already includes them
    if (std::ranges::none_of(types, [](const auto& t) { return t.bits > 64; })) {
        std::ranges::copy(diff::wide_int_types(), std::back_inserter(types));
    }
    return types;
}

auto checked_arithmetic() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : diff::int_types()) {
        for (const std::string_view op : {"+", "-", "*"}) {
            templates.emplace_back(binary(op, type, fmt::format("{{0}} {}% {{1}}", op)));
        }
        for (const std::string_view op : {"/", "%"}) { templates.emplace_back(binary(op, type)); }
    }
    return templates;
}

auto wrapping_saturating() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : int_types_with_wide()) {
        for (const std::string_view op : {"+%", "-%", "*%", "+|", "-|", "*|"}) {
            templates.emplace_back(binary(op, type));
        }
    }
    return templates;
}

auto bitwise_shifts() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : int_types_with_wide()) {
        for (const std::string_view op : {"&", "|", "^", "<<", ">>", "<<%", "<<|"}) {
            templates.emplace_back(binary(op, type));
        }
        templates.emplace_back(unary("~", type));
    }
    return templates;
}

auto int_comparisons() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : int_types_with_wide()) {
        for (const std::string_view op : {"==", "!=", "<", "<=", ">", ">="}) {
            templates.emplace_back(comparison(op, type));
        }
    }
    return templates;
}

auto int_negation() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : int_types_with_wide()) {
        if (type.kind != diff::scalar_kind::SIGNED) { continue; }
        templates.emplace_back(unary("-", type, "-%{0}"));
        templates.emplace_back(unary("-%", type));
    }
    return templates;
}

auto int_builtins() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : diff::int_types()) {
        // Just wide enough to hold the operand's width: `u4` for an 8-bit operand
        const auto count{diff::int_type(fmt::format("u{}", std::bit_width(type.bits)))};
        for (const std::string_view builtin :
             {"@min", "@max", "@divTrunc", "@divFloor", "@rem", "@mod"}) {
            templates.emplace_back(call(builtin, type, 2));
        }
        if (type.kind == diff::scalar_kind::SIGNED) {
            templates.emplace_back(call("@abs", type, 1));
        }
        for (const std::string_view builtin : {"@clz", "@ctz", "@popCount"}) {
            templates.emplace_back<diff::expr_template>({
                .text     = fmt::format("{}({{0}})", builtin),
                .operands = {type},
                .result   = count,
            });
        }
    }
    return templates;
}

auto int_casts() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    const auto                       types{int_types_with_wide()};
    for (const auto& from : types) {
        for (const auto& to : types) {
            templates.emplace_back<diff::expr_template>({
                .text     = fmt::format("@intCast({}, {{0}})", to.name),
                .operands = {from},
                .result   = to,
            });
            // Same-width `@truncate` is rejected in favor of `@bitCast`/`@intCast`
            if (to.bits < from.bits) {
                templates.emplace_back<diff::expr_template>({
                    .text     = fmt::format("@truncate({}, {{0}})", to.name),
                    .operands = {from},
                    .result   = to,
                });
            }
        }
    }
    return templates;
}

auto float_arithmetic() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : diff::host_float_types()) {
        for (const std::string_view op : {"+", "-", "*", "/", "%"}) {
            templates.emplace_back(binary(op, type));
        }
        for (const std::string_view op : {"==", "!=", "<", "<=", ">", ">="}) {
            templates.emplace_back(comparison(op, type));
        }
        templates.emplace_back(unary("-", type));
        for (const std::string_view builtin : {"@min", "@max"}) {
            templates.emplace_back(call(builtin, type, 2));
        }
        templates.emplace_back(call("@abs", type, 1));
    }
    return templates;
}

auto fused_multiply_add() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : diff::host_float_types()) {
        templates.emplace_back<diff::expr_template>({
            .text     = fmt::format("@mulAdd({}, {{0}}, {{1}}, {{2}})", type.name),
            .operands = {type, type, type},
            .result   = type,
        });
    }
    return templates;
}

auto float_int_conversions() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& f : diff::host_float_types()) {
        for (const auto& i : diff::int_types()) {
            templates.emplace_back<diff::expr_template>(
                {.text     = fmt::format("@floatFromInt({}, {{0}})", f.name),
                 .operands = {i},
                 .result   = f});
            templates.emplace_back<diff::expr_template>({
                .text     = fmt::format("@intFromFloat({}, {{0}})", i.name),
                .operands = {f},
                .result   = i,
            });
        }
    }
    return templates;
}

// A finite overflow is a fold error but rounds to an infinity when it runs, so nothing panics
auto float_casts() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& from : diff::host_float_types()) {
        for (const auto& to : diff::host_float_types()) {
            templates.emplace_back<diff::expr_template>({
                .text              = fmt::format("@floatCast({}, {{0}})", to.name),
                .operands          = {from},
                .result            = to,
                .fold_errors_panic = false,
            });
        }
    }
    return templates;
}

// Correctly rounded when folded; a runtime mismatch, once compiler_rt has the routines, is a bug
// in them. `@exp` past the range is a fold error but an infinity at runtime.
auto math_functions() -> std::vector<diff::expr_template> {
    std::vector<diff::expr_template> templates;
    for (const auto& type : diff::host_float_types()) {
        for (const std::string_view name : {"@sqrt",
                                            "@sin",
                                            "@cos",
                                            "@tan",
                                            "@exp",
                                            "@exp2",
                                            "@log",
                                            "@log2",
                                            "@log10",
                                            "@floor",
                                            "@ceil"}) {
            templates.push_back({
                .text              = fmt::format("{}({{0}})", name),
                .operands          = {type},
                .result            = type,
                .fold_errors_panic = false,
            });
        }
    }
    return templates;
}

} // namespace

// Operands of different types are converted to their peer type before they meet
auto mixed_type_peers() -> std::vector<diff::expr_template> {
    struct peer_pair {
        diff::scalar_type narrow;
        diff::scalar_type peer;
    };
    const std::vector<peer_pair> pairs{
        {diff::int_type("i8"), diff::int_type("i64")},
        {diff::int_type("u8"), diff::int_type("i16")},
        {diff::int_type("u32"), diff::int_type("i64")},
        {diff::int_type("i16"), diff::int_type("i33")},
        {diff::int_type("u8"), diff::int_type("usize")},
        {diff::int_type("i64"), diff::int_type("i128")},
        {diff::int_type("u64"), diff::int_type("i128")},
        {diff::int_type("i32"), diff::float_type("f64")},
        {diff::int_type("u8"), diff::float_type("f32")},
        {diff::float_type("f32"), diff::float_type("f64")},
    };

    std::vector<diff::expr_template> templates;
    for (const auto& [narrow, peer] : pairs) {
        const bool is_int{peer.kind != diff::scalar_kind::FLOAT};
        for (const std::string_view op : {"+", "-", "*"}) {
            // Both operand orders convert the narrow side
            for (const bool narrow_first : {true, false}) {
                templates.push_back({
                    .text = fmt::format("{{0}} {} {{1}}", op),
                    .operands =
                        narrow_first ? std::vector{narrow, peer} : std::vector{peer, narrow},
                    .result               = peer,
                    .unchecked_equivalent = is_int ? fmt::format("{{0}} {}% {{1}}", op) : "",
                });
            }
        }
        for (const std::string_view op : {"==", "<", ">="}) {
            templates.push_back({
                .text     = fmt::format("{{0}} {} {{1}}", op),
                .operands = {narrow, peer},
                .result   = diff::bool_type(),
            });
        }
        for (const std::string_view builtin : {"@min", "@max"}) {
            templates.push_back({
                .text     = fmt::format("{}({{0}}, {{1}})", builtin),
                .operands = {peer, narrow},
                .result   = peer,
            });
        }
        if (is_int) {
            templates.push_back({
                .text     = "{0} & {1}",
                .operands = {narrow, peer},
                .result   = peer,
            });
        }
    }
    return templates;
}

TEST_CASE("Checked integer arithmetic folds like it runs") {
    expect_agreement(checked_arithmetic());
}

TEST_CASE("Wrapping and saturating integer arithmetic folds like it runs") {
    expect_agreement(wrapping_saturating());
}

TEST_CASE("Bitwise operators and shifts fold like they run") { expect_agreement(bitwise_shifts()); }

TEST_CASE("Integer comparisons fold like they run") { expect_agreement(int_comparisons()); }

TEST_CASE("Integer negation folds like it runs") { expect_agreement(int_negation()); }

TEST_CASE("Integer builtins fold like they run") { expect_agreement(int_builtins()); }

TEST_CASE("Integer casts fold like they run") { expect_agreement(int_casts()); }

TEST_CASE("Float arithmetic folds like it runs") { expect_agreement(float_arithmetic()); }

TEST_CASE("Mixed-type operations fold like they run") { expect_agreement(mixed_type_peers()); }

TEST_CASE("Fused multiply-add folds with a single rounding") {
    expect_agreement(fused_multiply_add(), {.run = false});
}

TEST_CASE("Float and integer conversions fold like they run") {
    expect_agreement(float_int_conversions());
}

TEST_CASE("Float casts fold like they run") { expect_agreement(float_casts()); }

TEST_CASE("Math builtins fold like they run") { expect_agreement(math_functions()); }

// Only the host runs code, but every tier-1 target must fold the same operations
TEST_CASE("Every operation folds on the other tier-1 targets") {
    const auto uses_f80 = [](const diff::expr_template& tmpl) {
        if (tmpl.result.name == "f80") { return true; }
        return std::ranges::any_of(tmpl.operands,
                                   [](const diff::scalar_type& t) { return t.name == "f80"; });
    };
    std::vector<std::string_view> triples{"aarch64-unknown-linux-gnu"};
    if (get_env("GHOTI_DIFF_FULL")) {
        triples.insert(triples.end(), {"x86_64-unknown-linux-gnu", "aarch64-apple-darwin"});
    }
    for (const auto triple : triples) {
        const bool  has_f80{triple.starts_with("x86_64")};
        std::vector families{checked_arithmetic(), int_builtins(), float_arithmetic()};
        if (get_env("GHOTI_DIFF_FULL")) {
            families.emplace_back(int_casts());
            families.emplace_back(float_int_conversions());
            families.emplace_back(float_casts());
            families.emplace_back(math_functions());
        }
        for (auto templates : families) {
            if (!has_f80) { std::erase_if(templates, uses_f80); }
            INFO(triple);
            expect_agreement(templates, {.run = false, .triple = std::string{triple}});
        }
    }
}

// `zig build fuzz-semantics`: every family again with fresh random seeds until
// `GHOTI_DIFF_FUZZ_MINUTES` runs out. Each round's seed is in its failure message.
TEST_CASE("Semantics fuzz", "[.fuzz-semantics]") {
    const auto budget{get_env("GHOTI_DIFF_FUZZ_MINUTES")};
    const auto replay_seed{get_env("GHOTI_DIFF_FUZZ_SEED")};
    if (!budget && !replay_seed) { SKIP("set GHOTI_DIFF_FUZZ_MINUTES or GHOTI_DIFF_FUZZ_SEED"); }
    usize minutes{0};
    if (budget) { std::from_chars(budget->data(), budget->data() + budget->size(), minutes); }
    struct family {
        std::string_view                 name;
        std::vector<diff::expr_template> templates;
        bool                             run{true};
    };
    const std::vector<family> families{
        {"checked arithmetic", checked_arithmetic()},
        {"wrapping and saturating", wrapping_saturating()},
        {"bitwise and shifts", bitwise_shifts()},
        {"integer comparisons", int_comparisons()},
        {"integer negation", int_negation()},
        {"integer builtins", int_builtins()},
        {"integer casts", int_casts()},
        {"float arithmetic", float_arithmetic()},
        {"mixed-type peers", mixed_type_peers()},
        {"fused multiply-add", fused_multiply_add(), false},
        {"float and integer conversions", float_int_conversions()},
        {"float casts", float_casts()},
        {"math builtins", math_functions()},
    };

    // `GHOTI_DIFF_FUZZ_SEED` replays one failing seed through every family once
    stdx::option<u64> replay;
    if (replay_seed) {
        u64 seed{0};
        std::from_chars(replay_seed->data(), replay_seed->data() + replay_seed->size(), seed);
        replay.emplace(seed);
    }

    const auto         deadline{std::chrono::steady_clock::now() + std::chrono::minutes{minutes}};
    std::random_device entropy;
    usize              rounds{0};
    while (replay ? rounds < families.size() : std::chrono::steady_clock::now() < deadline) {
        const auto& fam{families[rounds % families.size()]};
        const u64   seed{replay ? *replay : (u64{entropy()} << 32) | entropy()};
        INFO(fmt::format("round {} ({}), GHOTI_DIFF_FUZZ_SEED={}", rounds, fam.name, seed));
        expect_agreement(fam.templates, {.run = fam.run, .seed = seed, .random_only = true});
        ++rounds;
    }
    fmt::println("semantics fuzz: {} rounds in {} minute(s)", rounds, minutes);
}

} // namespace ghoti::tests
