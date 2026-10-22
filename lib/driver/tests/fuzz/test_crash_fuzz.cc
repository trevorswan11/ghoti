#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "helpers/fuzz.hh"
#include "support/env.hh"
#include "support/subprocess.hh"

namespace ghoti::tests {

namespace {

constexpr usize MUTATIONS_PER_PROGRAM{3};
constexpr usize BATCH_SIZE{64};

[[nodiscard]] auto run_with(stdx::option<u32> exit_code, std::string output) -> fuzz::outcome {
    return {.name = "case", .exit_code = exit_code, .output = std::move(output)};
}

[[nodiscard]] auto parse_u64(std::string_view text) -> u64 {
    u64 value{0};
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

[[nodiscard]] auto repo_root() -> std::filesystem::path {
    const auto root{get_env("GHOTI_REPO_ROOT")};
    return root ? std::filesystem::path{*root} : std::filesystem::current_path();
}

auto print_summary(const fuzz::harness&        h,
                   const fuzz::failure_groups& groups,
                   std::string_view            summary) -> void {
    const auto text{fuzz::report(h, groups)};
    fmt::print("{}", text);
    fmt::println("{}", summary);
    INFO(text);
    CHECK(groups.empty());
}

} // namespace

TEST_CASE("Fuzz tokens concatenate back to their source") {
    const std::string_view source{
        "const s = \"a\\\"b\"; let x\t= @as(u8, 12);\n\"open\ncaf\xC3\xA9"};
    const auto  tokens{fuzz::tokenize(source)};
    std::string joined;
    for (const auto token : tokens) { joined += token; }
    CHECK(joined == source);

    CHECK(std::ranges::contains(tokens, std::string_view{"\"a\\\"b\""}));
    CHECK(std::ranges::contains(tokens, std::string_view{"@as"}));
    CHECK(std::ranges::contains(tokens, std::string_view{"12"}));
    CHECK(std::ranges::contains(tokens, std::string_view{" "}));
    // An unterminated quote is its own token, and a multibyte character stays whole
    CHECK(std::ranges::contains(tokens, std::string_view{"\""}));
    CHECK(tokens.back() == "\xC3\xA9");
}

TEST_CASE("Fuzz raw string extraction") {
    const std::string_view cc{R"cc(f(R"(one)"); g(R"gh(two ")" still)gh"); R"bad)cc"};
    const auto             bodies{fuzz::raw_strings(cc)};
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0] == "one");
    CHECK(bodies[1] == R"x(two ")" still)x");
}

TEST_CASE("Fuzz mutations are reproducible from their seed") {
    const std::vector<fuzz::fuzz_case> programs{
        {"a", "pub const main = fn(): i32 { return 0; };"},
        {"b", "const S = struct { x: i32, y: bool, };"},
    };
    const auto first{fuzz::mutated_cases(programs, 7, MUTATIONS_PER_PROGRAM)};
    const auto again{fuzz::mutated_cases(programs, 7, MUTATIONS_PER_PROGRAM)};
    REQUIRE(first.size() == programs.size() * MUTATIONS_PER_PROGRAM);
    for (usize i{0}; i < first.size(); ++i) {
        CHECK(first[i].name == again[i].name);
        CHECK(first[i].source == again[i].source);
    }
    CHECK(first.front().name == "s7_a_0");
}

TEST_CASE("Fuzz crash classification") {
    CHECK_FALSE(fuzz::is_compiler_bug(run_with(0U, "")));
    CHECK_FALSE(fuzz::is_compiler_bug(run_with(14U, "x.gh:1:2: error: unknown name")));
    CHECK_FALSE(fuzz::is_compiler_bug(run_with(14U, "error: @panic: message in source")));

    // A compile error exit with no reported error
    CHECK(fuzz::is_compiler_bug(run_with(14U, "warning: unused")));
    CHECK(fuzz::is_compiler_bug(run_with(3U, "")));
    CHECK(fuzz::is_compiler_bug(run_with(stdx::none, "")));
    CHECK(fuzz::is_compiler_bug(run_with(0U, "LLVM ERROR: out of registers")));
    CHECK(fuzz::is_compiler_bug(run_with(14U, "error: x\nthread 1 panic: oops")));
    CHECK(fuzz::is_compiler_bug(run_with(14U, "error\nf(int): C:\\src\\emit.cc:12:5: bad")));
    CHECK(fuzz::is_compiler_bug(run_with(14U, "error\nOperand must have the same type (i32)")));
    CHECK_FALSE(fuzz::is_compiler_bug(run_with(14U, "error\nf(int): C:\\src\\emit.cc:x:5")));
}

TEST_CASE("Fuzz signatures blank numbers at the earliest crash marker") {
    CHECK(fuzz::signature(run_with(14U, "error\nStack dump:\n0. at 0x1234\nLLVM ERROR: 12 bad")) ==
          "Stack dump:");
    CHECK(fuzz::signature(run_with(3U, "Assertion failed: i < 12, line 345\nmore")) ==
          "Assertion failed: i < N, line N");
    CHECK(fuzz::signature(run_with(3U, "first\nlast line\n\n")) == "exit=3 last line");
    CHECK(fuzz::signature(run_with(spawn_child_timeout_exit_code, "")) == "exit=TIMEOUT ");
}

// `zig build fuzz-crash`: mutated programs fed to the ghoti binary until `GHOTI_FUZZ_MINUTES` runs
// out, keeping every crashing case in `GHOTI_FUZZ_CASES`. `GHOTI_FUZZ_SEED` replays one round
TEST_CASE("Crash fuzz", "[.fuzz-crash]") {
    const auto budget{get_env("GHOTI_FUZZ_MINUTES")};
    const auto replay_seed{get_env("GHOTI_FUZZ_SEED")};
    if (!budget && !replay_seed) { SKIP("set GHOTI_FUZZ_MINUTES or GHOTI_FUZZ_SEED"); }

    const auto programs{fuzz::seed_programs(repo_root())};
    REQUIRE_FALSE(programs.empty());
    const auto h{fuzz::harness::from_env()};
    REQUIRE(std::filesystem::exists(h.ghoti));

    const auto start{std::chrono::steady_clock::now()};
    const auto deadline{start + std::chrono::minutes{budget ? parse_u64(*budget) : 0}};
    const auto out_of_time{
        [&] { return !replay_seed && std::chrono::steady_clock::now() >= deadline; }};

    u64                  seed{replay_seed ? parse_u64(*replay_seed) : u64{std::random_device{}()}};
    usize                total{0};
    fuzz::failure_groups groups;
    do {
        auto            cases{fuzz::mutated_cases(programs, seed, MUTATIONS_PER_PROGRAM)};
        std::mt19937_64 order{seed};
        std::ranges::shuffle(cases, order);
        for (usize i{0}; i < cases.size() && !out_of_time(); i += BATCH_SIZE) {
            const auto        end{std::min(cases.size(), i + BATCH_SIZE)};
            const std::vector batch(cases.begin() + static_cast<isize>(i),
                                    cases.begin() + static_cast<isize>(end));
            total += batch.size();
            fuzz::group_into(groups, h.run_all(batch));
        }
        ++seed;
    } while (!replay_seed && !out_of_time());

    const std::chrono::duration<f64, std::ratio<60>> elapsed{std::chrono::steady_clock::now() -
                                                             start};
    print_summary(h,
                  groups,
                  fmt::format("crash fuzz: {} bad groups, {} cases, {:.1f} min",
                              groups.size(),
                              total,
                              elapsed.count()));
}

// `zig build fuzz-crash -Dfuzz-replay=<dir>`: every .gh file in `GHOTI_FUZZ_REPLAY` run once, e.g.
// to recheck saved crashes. Copies run in `GHOTI_FUZZ_CASES`, so pointing both at the same
// directory drops the cases that no longer crash
TEST_CASE("Crash fuzz replay", "[.fuzz-replay]") {
    const auto replay_dir{get_env("GHOTI_FUZZ_REPLAY")};
    if (!replay_dir) { SKIP("set GHOTI_FUZZ_REPLAY"); }
    const auto h{fuzz::harness::from_env()};
    REQUIRE(std::filesystem::exists(h.ghoti));

    std::vector<std::filesystem::path> files;
    for (const auto& entry :
         std::filesystem::directory_iterator{std::filesystem::path{*replay_dir}}) {
        if (entry.is_regular_file() && entry.path().extension() == ".gh") {
            files.emplace_back(entry.path());
        }
    }
    std::ranges::sort(files);

    std::vector<fuzz::fuzz_case> cases;
    for (const auto& file : files) {
        std::ifstream in{file, std::ios::binary};
        cases.push_back({file.stem().string(), {std::istreambuf_iterator<char>{in}, {}}});
    }

    fuzz::failure_groups groups;
    fuzz::group_into(groups, h.run_all(cases));
    print_summary(h, groups, fmt::format("{} bad groups of {} cases", groups.size(), cases.size()));
}

} // namespace ghoti::tests
