#pragma once

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::tests::fuzz {

struct fuzz_case {
    std::string name;
    std::string source;
};

// Bodies of raw string literals (`R"tag(...)tag"`) in C++ source, in order of appearance
[[nodiscard]] auto raw_strings(std::string_view cc_source) -> std::vector<std::string_view>;

// Seed programs from the compiler tests' raw strings and examples/*.gh under `repo_root`
[[nodiscard]] auto seed_programs(const std::filesystem::path& repo_root) -> std::vector<fuzz_case>;

// Splits source into whitespace runs, identifiers, numbers, strings and single characters
[[nodiscard]] auto tokenize(std::string_view source) -> std::vector<std::string_view>;

// One random token-level edit: truncate, delete, duplicate, swap, replace, rename or splice
[[nodiscard]] auto mutate(std::string_view source, std::mt19937_64& rng) -> std::string;

// `per_program` mutations of every program, named after the seed, program and mutation index
[[nodiscard]] auto mutated_cases(const std::vector<fuzz_case>& programs,
                                 u64                           seed,
                                 usize per_program) -> std::vector<fuzz_case>;

struct outcome {
    std::string       name;
    stdx::option<u32> exit_code;
    std::string       output;
};

// Where the output of a compiler crash begins, if it has one
[[nodiscard]] auto find_crash_marker(std::string_view output) -> stdx::option<usize>;

// True when the run is a compiler bug rather than a clean success or a diagnostic
[[nodiscard]] auto is_compiler_bug(const outcome& run) -> bool;

// Groups failures by their crash site, with numbers blanked out
[[nodiscard]] auto signature(const outcome& run) -> std::string;

struct harness {
    std::filesystem::path ghoti;
    // Failing cases stay here for reproduction
    std::filesystem::path cases_dir;
    std::string           command{"build-obj"};
    usize                 jobs{8};

    // Built from `GHOTI` and `GHOTI_FUZZ_CASES`, defaulting to zig-out/bin/ghoti and a temp dir
    [[nodiscard]] static auto from_env() -> harness;

    [[nodiscard]] auto run_one(const fuzz_case& c) const -> outcome;
    // Every compiler bug among `cases`, run `jobs` at a time
    [[nodiscard]] auto run_all(const std::vector<fuzz_case>& cases) const -> std::vector<outcome>;
    [[nodiscard]] auto case_path(std::string_view name) const -> std::filesystem::path;
};

// Failure names by signature
using failure_groups = ankerl::unordered_dense::map<std::string, std::vector<outcome>>;

auto group_into(failure_groups& groups, std::vector<outcome> failures) -> void;

// The largest groups first, with up to four reproducer paths each
[[nodiscard]] auto report(const harness& h, const failure_groups& groups) -> std::string;

} // namespace ghoti::tests::fuzz
