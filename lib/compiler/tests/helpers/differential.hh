#pragma once

#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/int128.hh"

// Differential testing of compile-time folding against runtime evaluation. Every case is written
// twice: once with literal operands inside a `constexpr` (folded by `gir::const_eval`), and once
// with the same bit patterns loaded from `var` globals (computed by the emitted code). Operands and
// results cross the boundary as raw bit patterns so no literal parsing or formatting is involved.
namespace ghoti::tests::differential {

enum class scalar_kind : u8 {
    SIGNED,
    UNSIGNED,
    FLOAT,
    BOOL,
};

struct scalar_type {
    std::string name;
    scalar_kind kind;
    u16         bits;

    // The unsigned integer type holding this type's bit pattern (`u8` for `i8`, `u32` for `f32`)
    [[nodiscard]] auto bits_type() const -> std::string;
    [[nodiscard]] auto mask() const -> u128;
    [[nodiscard]] auto is_float() const noexcept -> bool { return kind == scalar_kind::FLOAT; }
    [[nodiscard]] auto is_int() const noexcept -> bool {
        return kind == scalar_kind::SIGNED || kind == scalar_kind::UNSIGNED;
    }
};

[[nodiscard]] auto int_type(std::string_view name) -> scalar_type;
[[nodiscard]] auto float_type(std::string_view name) -> scalar_type;
[[nodiscard]] auto bool_type() -> scalar_type;

// Integer types up to 64 bits, odd widths included; `GHOTI_DIFF_FULL=1` adds `wide_int_types()`
[[nodiscard]] auto int_types() -> std::vector<scalar_type>;
// Wider than 64 bits; their division and float conversions call compiler_rt routines
[[nodiscard]] auto wide_int_types() -> std::vector<scalar_type>;
// Floats whose arithmetic runs without compiler_rt on the x86_64 host
[[nodiscard]] auto host_float_types() -> std::vector<scalar_type>;

// An expression over `{0}`, `{1}`, ... operand placeholders, e.g. `{0} +% {1}` or `@min({0}, {1})`
struct expr_template {
    std::string              text;
    std::vector<scalar_type> operands;
    scalar_type              result;
    // With safety off, a fold error's runtime result must equal this expression's fold (`{0} +%
    // {1}` for `{0} + {1}`); empty skips the check because the unchecked result is undefined
    std::string unchecked_equivalent{};
};

using case_operands = std::vector<u128>;

// Boundary values (zero, one, min, max, powers of two, float specials, ...) and seeded random ones
[[nodiscard]] auto boundary_values(const scalar_type& type) -> std::vector<u128>;
[[nodiscard]] auto random_value(const scalar_type& type, std::mt19937_64& rng) -> u128;

// The seeded generator every harness run starts from; `GHOTI_DIFF_SEED` overrides the seed
[[nodiscard]] auto make_rng() -> std::mt19937_64;
// Extra random cases per template beyond the boundary products; `GHOTI_DIFF_ITERS` overrides it
[[nodiscard]] auto random_iterations() -> usize;

// Boundary products of every operand's boundary set, plus seeded random tuples
[[nodiscard]] auto make_cases(const expr_template& tmpl,
                              std::mt19937_64&     rng,
                              bool random_only = false) -> std::vector<case_operands>;

enum class fold_outcome : u8 {
    VALUE,         // folded to a constant
    COMPILE_ERROR, // the fold reported a diagnostic that runtime safety should mirror
    NOT_FOLDABLE,  // const_eval couldn't fold something every operation must fold
};

struct classified_case {
    case_operands operands;
    fold_outcome  outcome{fold_outcome::VALUE};
    std::string   diagnostic{};
};

// A divergence between the two evaluators, rendered for a failure message
struct mismatch {
    std::string description;
};

struct report {
    usize                    compared{0};
    usize                    fold_errors{0};
    usize                    panics_checked{0};
    std::vector<mismatch>    mismatches{};
    std::vector<std::string> not_foldable{};
    // compiler_rt routines the runtime half needs before it can run; empty once they exist
    std::vector<std::string> waiting_on{};
};

struct options {
    // Runtime evaluation needs compiler_rt routines the archive doesn't define yet
    bool run{true};
    // Fold errors whose runtime panic is checked, per template
    usize panic_samples{2};
    // Target triple for fold-only runs; empty is the host
    std::string triple{};
    // Overrides the fixed default seed
    stdx::option<u64> seed{};
    // Only random cases, for fuzz rounds after the boundary sets have been checked once
    bool random_only{false};
};

// Folds every case, runs the foldable ones, and checks that every fold error panics at runtime
// under safety. Templates are batched into as few programs as possible.
[[nodiscard]] auto check_all(const std::vector<expr_template>& templates, const options& opts = {})
    -> std::vector<report>;
[[nodiscard]] auto check(const expr_template& tmpl, const options& opts = {}) -> report;

// Renders a report's problems for a Catch2 failure message
[[nodiscard]] auto describe(const expr_template& tmpl, const report& rep) -> std::string;

} // namespace ghoti::tests::differential
