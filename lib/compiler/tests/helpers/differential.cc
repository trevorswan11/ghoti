#include "helpers/differential.hh"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <llvm/IR/LLVMContext.h>
#include <stdx/harness/hooks.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/diagnostic.hh"
#include "support/env.hh"
#include "support/int128.hh"
#include "support/subprocess.hh"
#include "support/tempfile.hh"

namespace ghoti::tests::differential {

namespace {

// Exit codes carry a failing index, so a program checks at most this many things
constexpr usize max_cases_per_program{250};
// Cases per runtime program across a batch of templates; bounds compile time
constexpr usize max_batch_cases{4'000};
// Above every case index an exit code can carry
constexpr u32 panic_exit_code{253};
static_assert(max_cases_per_program < panic_exit_code,
              "a case index must fit in a POSIX exit code");

// Turns a runtime panic into `panic_exit_code` instead of a slow trap. Linux has no libc to call,
// so it exits with a raw `exit_group` syscall.
[[nodiscard]] auto panic_prelude() -> std::string {
    return fmt::format(R"(@cfg(os == .windows) {{
    extern("kernel32") const ExitProcess: fn(code: u32): noreturn;
    const diff_exit = fn(code: u32): noreturn {{ ExitProcess(code); }};
}} else @cfg(os == .macos) {{
    extern("System", "exit") const sys_exit: fn(code: i32): noreturn;
    const diff_exit = fn(code: u32): noreturn {{ sys_exit(@bitCast(i32, code)); }};
}} else @cfg(arch == .x86_64) {{
    const diff_exit = fn(code: u32): noreturn {{
        _ = asm usize {{
            template: "syscall",
            outputs: ("={{rax}}" = _),
            inputs: ("{{rax}}" = 231uz, "{{rdi}}" = @as(usize, code)),
            clobbers: ("rcx", "r11", "memory"),
            options: (volatile),
        }};
        @trap();
    }};
}} else @cfg(arch == .aarch64) {{
    const diff_exit = fn(code: u32): noreturn {{
        _ = asm usize {{
            template: "svc #0",
            outputs: ("={{x0}}" = _),
            inputs: ("{{x8}}" = 94uz, "{{x0}}" = @as(usize, code)),
            clobbers: ("memory"),
            options: (volatile),
        }};
        @trap();
    }};
}}
pub const panic_handler = fn(_: []u8, _: builtin.SourceLocation): noreturn {{ diff_exit({}); }};
)",
                       panic_exit_code);
}

// `GHOTI_DIFF_FULL=1` checks every boundary value and more random cases; the default subset
// keeps the suite quick
[[nodiscard]] auto full_depth() -> bool {
    static const bool full{get_env("GHOTI_DIFF_FULL").has_value()};
    return full;
}

// `GHOTI_DIFF_TRACE=1` prints how long each phase of a run takes
class phase_timer {
  public:
    explicit phase_timer(std::string label)
        : label_{std::move(label)}, start_{std::chrono::steady_clock::now()} {}
    ~phase_timer() {
        static const bool enabled{get_env("GHOTI_DIFF_TRACE").has_value()};
        if (!enabled) { return; }
        const std::chrono::duration<double> elapsed{std::chrono::steady_clock::now() - start_};
        fmt::println(stderr, "[diff] {:8.3f}s  {}", elapsed.count(), label_);
    }
    MAKE_PINNED(phase_timer);

  private:
    std::string                           label_;
    std::chrono::steady_clock::time_point start_;
};

// A float format's field widths; f80 stores its integer bit explicitly
struct float_layout {
    u16  exponent_bits;
    u16  mantissa_bits;
    bool explicit_integer_bit;
};

[[nodiscard]] auto layout_of(const scalar_type& type) -> float_layout {
    switch (type.bits) {
    case 16: return {5, 10, false};
    case 32: return {8, 23, false};
    case 64: return {11, 52, false};
    case 80: return {15, 64, true};
    default: return {15, 112, false};
    }
}

[[nodiscard]] auto ones(u16 count) -> u128 {
    return count >= 128 ? ~u128{0} : (u128{1} << count) - 1;
}

// Assembles a float's bit pattern; `mantissa` excludes f80's explicit integer bit
[[nodiscard]] auto float_bits(const scalar_type& type, bool negative, u128 exponent, u128 mantissa)
    -> u128 {
    const auto layout{layout_of(type)};
    u128       bits{mantissa &
              ones(layout.explicit_integer_bit ? layout.mantissa_bits - 1 : layout.mantissa_bits)};
    if (layout.explicit_integer_bit && exponent != 0) {
        bits |= u128{1} << (layout.mantissa_bits - 1);
    }
    bits |= exponent << layout.mantissa_bits;
    if (negative) { bits |= u128{1} << (layout.mantissa_bits + layout.exponent_bits); }
    return bits;
}

[[nodiscard]] auto float_boundaries(const scalar_type& type) -> std::vector<u128> {
    const auto layout{layout_of(type)};
    const auto fraction_bits{static_cast<u16>(layout.explicit_integer_bit ? layout.mantissa_bits - 1
                                                                          : layout.mantissa_bits)};
    const u128 bias{ones(static_cast<u16>(layout.exponent_bits - 1))};
    const u128 max_exponent{ones(layout.exponent_bits)};
    const u128 all_fraction{ones(fraction_bits)};
    const u128 quiet_bit{u128{1} << (fraction_bits - 1)};

    std::vector<u128> values;
    if (!full_depth()) {
        return {float_bits(type, false, 0, 0),
                float_bits(type, true, 0, 0),
                float_bits(type, false, 0, 1),
                float_bits(type, false, bias, 0),
                float_bits(type, false, bias, 1),
                float_bits(type, true, bias, quiet_bit),
                float_bits(type, false, max_exponent - 1, all_fraction),
                float_bits(type, false, max_exponent, 0),
                float_bits(type, true, max_exponent, 0),
                float_bits(type, false, max_exponent, quiet_bit)};
    }
    for (const bool negative : {false, true}) {
        values.emplace_back(float_bits(type, negative, 0, 0));                   // zero
        values.emplace_back(float_bits(type, negative, 0, 1));                   // min subnormal
        values.emplace_back(float_bits(type, negative, 0, all_fraction));        // max subnormal
        values.emplace_back(float_bits(type, negative, 1, 0));                   // min normal
        values.emplace_back(float_bits(type, negative, bias, 0));                // 1.0
        values.emplace_back(float_bits(type, negative, bias, 1));                // 1 + ulp
        values.emplace_back(float_bits(type, negative, bias - 1, all_fraction)); // 1 - ulp/2
        values.emplace_back(float_bits(type, negative, bias, quiet_bit));        // 1.5
        values.emplace_back(float_bits(type, negative, bias + 1, 0));            // 2.0
        values.emplace_back(float_bits(type, negative, max_exponent - 1, all_fraction)); // max
        values.emplace_back(float_bits(type, negative, max_exponent, 0));                // inf
    }
    values.emplace_back(float_bits(type, false, max_exponent, quiet_bit)); // quiet NaN
    return values;
}

[[nodiscard]] auto int_boundaries(const scalar_type& type) -> std::vector<u128> {
    const auto        w{type.bits};
    const auto        m{type.mask()};
    std::vector<u128> values{0, 1, 2, m, m - 1};
    const u128        top{u128{1} << (w - 1)};
    values.insert(values.end(), {top, top - 1});
    if (full_depth()) {
        values.insert(values.end(), {top + 1, top - 2});
        for (const u16 k : {static_cast<u16>(w / 2), static_cast<u16>(w > 2 ? w - 2 : 1)}) {
            const u128 p{u128{1} << k};
            values.insert(values.end(), {p, p - 1, (~p + 1) & m, (p + 1) & m});
        }
    } else {
        values.emplace_back(u128{1} << (w / 2));
    }
    for (auto& v : values) { v &= m; }
    std::ranges::sort(values);
    const auto dup{std::ranges::unique(values)};
    values.erase(dup.begin(), dup.end());
    return values;
}

[[nodiscard]] auto u128_text(u128 value) -> std::string { return fmt::format("{}", value); }

[[nodiscard]] auto hex(u128 value) -> std::string {
    constexpr std::string_view digits{"0123456789abcdef"};
    std::string                out;
    do {
        out.insert(out.begin(), digits[static_cast<usize>(value & 0xF)]);
        value >>= 4;
    } while (value != 0);
    return "0x" + out;
}

[[nodiscard]] auto storage_type(const scalar_type& type) -> std::string {
    return type.kind == scalar_kind::BOOL ? std::string{"u8"} : type.bits_type();
}

[[nodiscard]] auto result_width(const scalar_type& type) -> u16 {
    return type.kind == scalar_kind::BOOL ? u16{8} : type.bits;
}

// `@bitCast(i8, @as(u8, 200))`: an exact operand with no literal parsing involved
[[nodiscard]] auto literal(const scalar_type& type, u128 bits) -> std::string {
    if (type.kind == scalar_kind::BOOL) { return bits != 0 ? "true" : "false"; }
    return fmt::format("@bitCast({}, @as({}, {}))", type.name, type.bits_type(), u128_text(bits));
}

[[nodiscard]] auto runtime_operand(const scalar_type& type, std::string_view storage)
    -> std::string {
    if (type.kind == scalar_kind::BOOL) { return fmt::format("({} != 0)", storage); }
    return fmt::format("@bitCast({}, {})", type.name, storage);
}

// Replaces `{0}`, `{1}`, ... with the given operand texts
[[nodiscard]] auto instantiate(std::string_view text, const std::vector<std::string>& operands)
    -> std::string {
    std::string out;
    for (usize i{0}; i < text.size(); ++i) {
        if (text[i] == '{' && i + 2 < text.size() && text[i + 2] == '}' && text[i + 1] >= '0' &&
            text[i + 1] <= '9') {
            out += operands[static_cast<usize>(text[i + 1] - '0')];
            i += 2;
            continue;
        }
        out += text[i];
    }
    return out;
}

// The result as its bit pattern, the only form that crosses between the two evaluators
[[nodiscard]] auto as_result_bits(const scalar_type& result, std::string_view expr) -> std::string {
    if (result.kind == scalar_kind::BOOL) { return fmt::format("@intFromBool(u8, {})", expr); }
    return fmt::format("@bitCast({}, ({}))", result.bits_type(), expr);
}

[[nodiscard]] auto folded_expr(std::string_view     text,
                               const expr_template& tmpl,
                               const case_operands& operands) -> std::string {
    std::vector<std::string> texts;
    for (usize i{0}; i < operands.size(); ++i) {
        texts.emplace_back(literal(tmpl.operands[i], operands[i]));
    }
    return instantiate(text, texts);
}

[[nodiscard]] auto runtime_expr(const expr_template& tmpl,
                                std::string_view     prefix,
                                std::string_view     index) -> std::string {
    std::vector<std::string> texts;
    for (usize j{0}; j < tmpl.operands.size(); ++j) {
        texts.emplace_back(
            runtime_operand(tmpl.operands[j], fmt::format("{}{}[{}]", prefix, j, index)));
    }
    return instantiate(tmpl.text, texts);
}

[[nodiscard]] auto describe_operands(const expr_template& tmpl, const case_operands& operands)
    -> std::string {
    std::vector<std::string> parts;
    for (usize i{0}; i < operands.size(); ++i) {
        parts.emplace_back(fmt::format("{}={}", tmpl.operands[i].name, hex(operands[i])));
    }
    return fmt::format("{}", fmt::join(parts, ", "));
}

// `const <prefix>{j}: [N]U = .{...};` for every operand of the given cases
[[nodiscard]] auto operand_arrays(const expr_template&             tmpl,
                                  gsl::span<const classified_case> cases,
                                  std::string_view                 prefix) -> std::string {
    std::string out;
    for (usize j{0}; j < tmpl.operands.size(); ++j) {
        std::vector<std::string> values;
        for (const auto& c : cases) { values.emplace_back(u128_text(c.operands[j])); }
        out += fmt::format("let mut {}{}: [{}]{} = .{{ {} }};\n",
                           prefix,
                           j,
                           cases.size(),
                           storage_type(tmpl.operands[j]),
                           fmt::join(values, ", "));
    }
    return out;
}

// The folded results of `text` for each case, as a `const` array of bit patterns
[[nodiscard]] auto want_array(std::string_view                 name,
                              std::string_view                 text,
                              const expr_template&             tmpl,
                              gsl::span<const classified_case> cases) -> std::string {
    std::vector<std::string> wants;
    for (const auto& c : cases) {
        wants.emplace_back(as_result_bits(tmpl.result, folded_expr(text, tmpl, c.operands)));
    }
    return fmt::format("const {}: [{}]{} = .{{\n    {}\n}};\n",
                       name,
                       cases.size(),
                       storage_type(tmpl.result),
                       fmt::join(wants, ",\n    "));
}

// A NaN-aware comparison of `got` against `want[i]`
[[nodiscard]] auto differs_condition(const scalar_type& result, std::string_view want)
    -> std::string {
    std::string differs{fmt::format("{} != {}", as_result_bits(result, "got"), want)};
    if (!result.is_float()) { return differs; }
    return fmt::format("{0} and !(got != got and @bitCast({1}, {2}) != @bitCast({1}, {2}))",
                       differs,
                       result.name,
                       want);
}

// `GHOTI_DIFF_DUMP=<path>` keeps the most recent generated program there, for debugging
auto dump_program(std::string_view source) -> void {
    if (const auto path{get_env("GHOTI_DIFF_DUMP")}) {
        std::ofstream out{std::string{*path}};
        out << source;
    }
}

// A program's exit code, or the compiler builtins it couldn't link without
struct run_result {
    stdx::option<u32>        exit_code{};
    std::vector<std::string> missing_builtins{};
};

[[nodiscard]] auto try_run(std::string_view source) -> run_result {
    dump_program(source);
    auto              ctx_idx{helpers::type_check_and_verify(source)};
    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const tempfile exe{
        std::in_place,
        fmt::format("{}{}", tempfile::make_temp_path("diff_run").string(), extension)};
    const auto emitted{helpers::emit_executable(*ctx_idx.first, context, exe.path)};
    if (!emitted) {
        // Only a link that fails for want of compiler_rt routines is expected
        const auto message{emitted.error().get_message().value_or("")};
        if (!message.contains("compiler builtin")) {
            fmt::println("{}", emitted.error());
            REQUIRE(emitted);
        }
        run_result                 result;
        constexpr std::string_view marker{"undefined symbol: "};
        for (usize pos{message.find(marker)}; pos != std::string::npos;
             pos = message.find(marker, pos + marker.size())) {
            const auto start{pos + marker.size()};
            result.missing_builtins.emplace_back(
                message.substr(start, message.find_first_of(" \r\n", start) - start));
        }
        return result;
    }
    return {.exit_code =
                spawn_child(mock_argv{exe.path.string()}).transform(helpers::portable_exit_code)};
}

// Folds every case of every template as a module constant and reads back which ones reported a
// diagnostic.
auto classify_all(const std::vector<expr_template>&              templates,
                  const std::vector<std::vector<case_operands>>& cases,
                  const options& opts) -> std::vector<std::vector<classified_case>> {
    std::vector<std::vector<classified_case>> classified(templates.size());
    std::vector<std::pair<usize, usize>>      pending;
    for (usize t{0}; t < templates.size(); ++t) {
        for (usize k{0}; k < cases[t].size(); ++k) {
            classified[t].emplace_back<classified_case>({.operands = cases[t][k]});
            pending.emplace_back(t, k);
        }
    }

    while (!pending.empty()) {
        const phase_timer timer{fmt::format("classify {} cases", pending.size())};
        std::string       source;
        for (const auto [t, k] : pending) {
            source += fmt::format(
                "const fold_{}_{}: {} = {};\n",
                t,
                k,
                storage_type(templates[t].result),
                as_result_bits(templates[t].result,
                               folded_expr(templates[t].text, templates[t], cases[t][k])));
        }

        dump_program(source);
        auto [ctx, idx]{opts.triple.empty() ? helpers::type_check(source)
                                            : helpers::type_check_for_target(source, opts.triple)};
        const auto diags{ctx->root_mod.diagnostics.as_opt<sema::diagnostics>()};
        if (!diags) { break; }
        bool reported{false};
        for (const auto& diag : *diags) {
            const auto snap{diag.snapshot()};
            if (snap.level != diagnostic_level::ERROR || !snap.location) { continue; }
            if (snap.location->line >= pending.size()) { continue; }
            const auto [t, k]{pending[snap.location->line]};
            auto& target{classified[t][k]};
            if (target.outcome != fold_outcome::VALUE) { continue; }
            // Folding reports its own failures with these kinds; anything else means the fold
            // never happened (or the template is ill-typed)
            const bool fold_error{(diag.get_error() == sema::error::COMPTIME_EVALUATION_FAILED ||
                                   diag.get_error() == sema::error::LITERAL_OUT_OF_RANGE) &&
                                  !snap.message.contains("must be a constant expression")};
            target.outcome = fold_error ? fold_outcome::COMPILE_ERROR : fold_outcome::NOT_FOLDABLE;
            target.diagnostic = snap.message;
            reported          = true;
        }
        if (!reported) { break; }
        std::erase_if(pending, [&](const auto& owner) {
            return classified[owner.first][owner.second].outcome != fold_outcome::VALUE;
        });
    }
    return classified;
}

// Reads a value's bits back a byte at a time through the exit code; none when computing it panics
[[nodiscard]] auto read_bits(const scalar_type& result,
                             std::string_view   prelude,
                             std::string_view   value) -> stdx::option<u128> {
    u128 bits{0};
    for (u16 byte{0}; byte * 8 < result_width(result); ++byte) {
        const auto code{
            helpers::compile_and_run(fmt::format("{}{}pub const main = fn(): i32 {{\n"
                                                 "    let got = {};\n"
                                                 "    return @intCast(i32, ({} >> {}) & 0xFF);\n"
                                                 "}};\n",
                                                 panic_prelude(),
                                                 prelude,
                                                 value,
                                                 as_result_bits(result, "got"),
                                                 byte * 8))};
        if (code == panic_exit_code) { return stdx::none; }
        bits |= u128{code} << (byte * 8);
    }
    return bits;
}

[[nodiscard]] auto scalar_globals(const expr_template& tmpl, const case_operands& operands)
    -> std::string {
    std::string globals;
    for (usize i{0}; i < operands.size(); ++i) {
        globals += fmt::format("let mut v{}: [1]{} = .{{ {} }};\n",
                               i,
                               storage_type(tmpl.operands[i]),
                               u128_text(operands[i]));
    }
    return globals;
}

[[nodiscard]] auto render_mismatch(const expr_template& tmpl, const classified_case& c)
    -> mismatch {
    const auto folded{read_bits(tmpl.result, "", folded_expr(tmpl.text, tmpl, c.operands))};
    const auto runtime{
        read_bits(tmpl.result, scalar_globals(tmpl, c.operands), runtime_expr(tmpl, "v", "0"))};
    return mismatch{fmt::format("{} [{}]: folded {}, runtime {}",
                                tmpl.text,
                                describe_operands(tmpl, c.operands),
                                folded ? hex(*folded) : std::string{"<panic>"},
                                runtime ? hex(*runtime) : std::string{"<panic>"})};
}

// Runs one template's cases: 0 when all agree, the failing case + 1, or the panic code
[[nodiscard]] auto run_cases(const expr_template& tmpl, gsl::span<const classified_case> cases)
    -> u32 {
    return helpers::compile_and_run(
        fmt::format("{}{}{}pub const main = fn(): i32 {{\n"
                    "    let mut i: usize = 0;\n"
                    "    while (i < {}) : (i += 1) {{\n"
                    "        let got = {};\n"
                    "        if ({}) {{ return @intCast(i32, i) + 1; }}\n"
                    "    }}\n"
                    "    return 0;\n"
                    "}};\n",
                    panic_prelude(),
                    operand_arrays(tmpl, cases, "op"),
                    want_array("want", tmpl.text, tmpl, cases),
                    cases.size(),
                    runtime_expr(tmpl, "op", "i"),
                    differs_condition(tmpl.result, "want[i]")));
}

// The first mismatching case of one template; a panicking chunk is split until the case is found
[[nodiscard]] auto locate_mismatch(const expr_template&             tmpl,
                                   gsl::span<const classified_case> cases)
    -> stdx::option<mismatch> {
    for (usize start{0}; start < cases.size(); start += max_cases_per_program) {
        const auto chunk{
            cases.subspan(start, std::min(max_cases_per_program, cases.size() - start))};
        const auto code{run_cases(tmpl, chunk)};
        if (code == 0) { continue; }
        if (code != panic_exit_code && code <= chunk.size()) {
            return render_mismatch(tmpl, chunk[code - 1]);
        }
        if (chunk.size() == 1) { return render_mismatch(tmpl, chunk.front()); }
        const auto half{chunk.size() / 2};
        if (auto found{locate_mismatch(tmpl, chunk.first(half))}) { return found; }
        return locate_mismatch(tmpl, chunk.subspan(half));
    }
    return stdx::none;
}

// One program checks every template of a batch: none when all agree, the index of the first that
// disagrees, or the batch size when something panicked. Missing builtins come back separately.
[[nodiscard]] auto run_batch(const std::vector<const expr_template*>&             templates,
                             const std::vector<gsl::span<const classified_case>>& foldable,
                             std::vector<std::string>& missing) -> stdx::option<usize> {
    std::string source{panic_prelude()};
    std::string main_body;
    for (usize t{0}; t < templates.size(); ++t) {
        const auto& tmpl{*templates[t]};
        const auto  cases{foldable[t]};
        if (cases.empty()) { continue; }
        const auto prefix{fmt::format("t{}_op", t)};
        source += operand_arrays(tmpl, cases, prefix);
        source += want_array(fmt::format("t{}_want", t), tmpl.text, tmpl, cases);
        source += fmt::format("const check_{0} = fn(): bool {{\n"
                              "    let mut i: usize = 0;\n"
                              "    while (i < {1}) : (i += 1) {{\n"
                              "        let got = {2};\n"
                              "        if ({3}) {{ return false; }}\n"
                              "    }}\n"
                              "    return true;\n"
                              "}};\n",
                              t,
                              cases.size(),
                              runtime_expr(tmpl, prefix, "i"),
                              differs_condition(tmpl.result, fmt::format("t{}_want[i]", t)));
        main_body += fmt::format("    if (!check_{}()) {{ return {}; }}\n", t, t + 1);
    }
    const auto run{try_run(
        fmt::format("{}pub const main = fn(): i32 {{\n{}    return 0;\n}};\n", source, main_body))};
    if (!run.missing_builtins.empty()) {
        missing = run.missing_builtins;
        return stdx::none;
    }
    const auto code{run.exit_code.value_or(panic_exit_code)};
    if (code == 0) { return stdx::none; }
    if (code == panic_exit_code || code > templates.size()) { return templates.size(); }
    return static_cast<usize>(code - 1);
}

// The compiler builtins one template's runtime evaluation needs that compiler_rt lacks
[[nodiscard]] auto template_needs(const expr_template& tmpl, const classified_case& sample)
    -> std::vector<std::string> {
    const gsl::span one{&sample, 1};
    // The result must be used: instruction selection drops a dead `frem` and its `fmod` call
    return helpers::missing_builtins(fmt::format("{}pub const main = fn(): i32 {{\n"
                                                 "    let got = {};\n"
                                                 "    return @intCast(i32, {} & 1);\n"
                                                 "}};\n",
                                                 operand_arrays(tmpl, one, "op"),
                                                 runtime_expr(tmpl, "op", "0"),
                                                 as_result_bits(tmpl.result, "got")));
}

struct error_sample {
    usize           template_index;
    classified_case sample;
};

// One case's exit code. A freshly linked executable can be briefly locked by a scanner, so a
// spawn that never started gets a second try
[[nodiscard]] auto
run_error_case(const std::filesystem::path& exe, std::string_view mode, const std::string& which) {
    const auto spawn{[&] {
        return spawn_child(mock_argv{exe.string(), std::string{mode}, which})
            .transform(helpers::portable_exit_code);
    }};
    const auto first{spawn()};
    return first ? first : spawn();
}

// Every sampled fold error in one executable, run once per case: under safety it must panic, and
// with safety off it must produce the template's unchecked equivalent
auto check_errors(const std::vector<expr_template>& templates,
                  const std::vector<error_sample>&  samples,
                  std::vector<report>&              reports) -> void {
    if (samples.empty()) { return; }

    std::string source{
        panic_prelude() +
        "const parse_index = fn(text: [:0]u8): usize {\n"
        "    let mut value: usize = 0;\n"
        "    let mut p: usize = 0;\n"
        "    while (p < text.len) : (p += 1) { value = value * 10 + @as(usize, text[p] - 48); }\n"
        "    return value;\n"
        "};\n"};
    std::string dispatch;
    for (usize s{0}; s < samples.size(); ++s) {
        const auto&     tmpl{templates[samples[s].template_index]};
        const gsl::span one{&samples[s].sample, 1};
        const auto      prefix{fmt::format("e{}_op", s)};
        source += operand_arrays(tmpl, one, prefix);
        const auto expr{runtime_expr(tmpl, prefix, "0")};
        source += fmt::format(
            "const safe_{} = fn(): i32 {{ let got = {}; _ = got; return 0; }};\n", s, expr);
        dispatch +=
            fmt::format("    if (mode == 0 and which == {0}) {{ return safe_{0}(); }}\n", s);
        if (!tmpl.unchecked_equivalent.empty()) {
            source += want_array(fmt::format("e{}_want", s), tmpl.unchecked_equivalent, tmpl, one);
            source += fmt::format("const unsafe_{0} = fn(): i32 {{\n"
                                  "    @setRuntimeSafety(false);\n"
                                  "    let got = {1};\n"
                                  "    if ({2}) {{ return 1; }}\n"
                                  "    return 0;\n"
                                  "}};\n",
                                  s,
                                  expr,
                                  differs_condition(tmpl.result, fmt::format("e{}_want[0]", s)));
            dispatch +=
                fmt::format("    if (mode == 1 and which == {0}) {{ return unsafe_{0}(); }}\n", s);
        }
    }
    source += fmt::format("pub const main = fn(args: [][:0]u8): i32 {{\n"
                          "    let mode = parse_index(args[1]);\n"
                          "    let which = parse_index(args[2]);\n"
                          "{}"
                          "    return 99;\n"
                          "}};\n",
                          dispatch);

    dump_program(source);
    auto              ctx_idx{helpers::type_check_and_verify(source)};
    llvm::LLVMContext context;
    const auto extension{codegen::get_default_output_extension(codegen::output_type::EXECUTABLE)};
    const tempfile exe{
        std::in_place,
        fmt::format("{}{}", tempfile::make_temp_path("differential").string(), extension)};
    const auto emitted{helpers::emit_executable(*ctx_idx.first, context, exe.path)};
    if (!emitted) { fmt::println("{}", emitted.error()); }
    REQUIRE(emitted);

    for (usize s{0}; s < samples.size(); ++s) {
        const auto& [t, sample]{samples[s]};
        const auto& tmpl{templates[t]};
        auto&       rep{reports[t]};
        const auto  which{std::to_string(s)};

        ++rep.panics_checked;
        const auto panicked{run_error_case(exe.path, "0", which)};
        if (panicked != panic_exit_code) {
            rep.mismatches.emplace_back<mismatch>({fmt::format(
                "{} [{}]: folding reports \"{}\" but runtime with safety on exited {} instead of "
                "panicking",
                tmpl.text,
                describe_operands(tmpl, sample.operands),
                sample.diagnostic,
                panicked ? static_cast<i64>(*panicked) : -1)});
        }
        if (tmpl.unchecked_equivalent.empty()) { continue; }
        const auto unchecked{run_error_case(exe.path, "1", which)};
        if (unchecked != 0U) {
            rep.mismatches.emplace_back<mismatch>({fmt::format(
                "{} [{}]: with safety off the result isn't the unchecked `{}` (exited {})",
                tmpl.text,
                describe_operands(tmpl, sample.operands),
                tmpl.unchecked_equivalent,
                unchecked ? static_cast<i64>(*unchecked) : -1)});
        }
    }
}

} // namespace

auto scalar_type::bits_type() const -> std::string {
    if (kind == scalar_kind::BOOL) { return "u8"; }
    if (name == "isize" || name == "usize") { return "usize"; }
    return fmt::format("u{}", bits);
}

auto scalar_type::mask() const -> u128 { return ones(kind == scalar_kind::BOOL ? 1 : bits); }

auto int_type(std::string_view name) -> scalar_type {
    if (name == "isize") { return {std::string{name}, scalar_kind::SIGNED, 64}; }
    if (name == "usize") { return {std::string{name}, scalar_kind::UNSIGNED, 64}; }
    u16 bits{0};
    std::from_chars(name.data() + 1, name.data() + name.size(), bits);
    return {
        std::string{name}, name.front() == 'i' ? scalar_kind::SIGNED : scalar_kind::UNSIGNED, bits};
}

auto float_type(std::string_view name) -> scalar_type {
    u16 bits{0};
    std::from_chars(name.data() + 1, name.data() + name.size(), bits);
    return {std::string{name}, scalar_kind::FLOAT, bits};
}

auto bool_type() -> scalar_type { return {"bool", scalar_kind::BOOL, 1}; }

auto int_types() -> std::vector<scalar_type> {
    std::vector<scalar_type> types;
    // Both signednesses, every power-of-two width, pointer width, and odd widths either side of 32
    std::vector<std::string_view> names{"i8", "u8", "i16", "u32", "i64", "usize", "i7", "i33"};
    if (full_depth()) { names.insert(names.end(), {"u16", "i32", "u64", "isize", "u13"}); }
    for (const auto name : names) { types.emplace_back(int_type(name)); }
    if (full_depth()) { std::ranges::copy(wide_int_types(), std::back_inserter(types)); }
    return types;
}

auto wide_int_types() -> std::vector<scalar_type> {
    return {int_type("i128"), int_type("u128"), int_type("u65")};
}

auto host_float_types() -> std::vector<scalar_type> {
#if GHOTI_ASM_HOST_X86_64
    return {float_type("f32"), float_type("f64"), float_type("f80")};
#else
    return {float_type("f32"), float_type("f64")};
#endif
}

auto boundary_values(const scalar_type& type) -> std::vector<u128> {
    switch (type.kind) {
    case scalar_kind::FLOAT: return float_boundaries(type);
    case scalar_kind::BOOL:  return {0, 1};
    default:                 return int_boundaries(type);
    }
}

auto random_value(const scalar_type& type, std::mt19937_64& rng) -> u128 {
    const u128 raw{(u128{rng()} << 64) | u128{rng()}};
    if (!type.is_float()) { return raw & type.mask(); }
    // Half anywhere, half near 1.0 where rounding is interesting
    if (rng() % 2 == 0) { return raw & type.mask(); }
    const auto layout{layout_of(type)};
    const u128 bias{ones(static_cast<u16>(layout.exponent_bits - 1))};
    const auto exponent{bias - 8 + u128{rng() % 17}};
    return float_bits(type, rng() % 2 == 0, exponent, raw);
}

auto make_rng() -> std::mt19937_64 {
    u64 seed{0x9e3779b97f4a7c15ULL};
    if (const auto env{get_env("GHOTI_DIFF_SEED")}) {
        std::from_chars(env->data(), env->data() + env->size(), seed);
    }
    return std::mt19937_64{seed};
}

auto random_iterations() -> usize {
    usize iters{full_depth() ? 32UZ : 8UZ};
    if (const auto env{get_env("GHOTI_DIFF_ITERS")}) {
        std::from_chars(env->data(), env->data() + env->size(), iters);
    }
    return iters;
}

auto make_cases(const expr_template& tmpl, std::mt19937_64& rng, bool random_only)
    -> std::vector<case_operands> {
    std::vector<case_operands>     cases;
    std::vector<std::vector<u128>> boundaries;
    for (const auto& type : tmpl.operands) { boundaries.emplace_back(boundary_values(type)); }

    // The full boundary product for one or two operands; random picks beyond that, and in fuzz
    // rounds, which have already checked the product once
    if (!random_only && tmpl.operands.size() <= 2) {
        cases.emplace_back();
        for (const auto& set : boundaries) {
            std::vector<case_operands> next;
            for (const auto& partial : cases) {
                for (const auto v : set) {
                    auto grown{partial};
                    grown.emplace_back(v);
                    next.emplace_back(std::move(grown));
                }
            }
            cases = std::move(next);
        }
    } else {
        for (usize n{0}; n < 64; ++n) {
            case_operands c;
            for (const auto& set : boundaries) { c.emplace_back(set[rng() % set.size()]); }
            cases.emplace_back(std::move(c));
        }
    }
    for (usize n{0}; n < random_iterations(); ++n) {
        case_operands c;
        for (const auto& type : tmpl.operands) { c.emplace_back(random_value(type, rng)); }
        cases.emplace_back(std::move(c));
    }
    return cases;
}

auto check_all(const std::vector<expr_template>& templates, const options& opts)
    -> std::vector<report> {
    // Thousands of throwaway compilations; tracking each of their allocations dominates the run
    stdx::untracked_scope untracked_guard;
    auto                  rng{opts.seed ? std::mt19937_64{*opts.seed} : make_rng()};
    std::vector<std::vector<case_operands>> cases;
    for (const auto& tmpl : templates) {
        cases.emplace_back(make_cases(tmpl, rng, opts.random_only));
    }
    const auto classified{classify_all(templates, cases, opts)};

    std::vector<report>                       reports(templates.size());
    std::vector<std::vector<classified_case>> foldable(templates.size());
    std::vector<error_sample>                 samples;
    for (usize t{0}; t < templates.size(); ++t) {
        std::vector<classified_case> errors;
        for (const auto& c : classified[t]) {
            switch (c.outcome) {
            case fold_outcome::VALUE:         foldable[t].emplace_back(c); break;
            case fold_outcome::COMPILE_ERROR: errors.emplace_back(c); break;
            case fold_outcome::NOT_FOLDABLE:
                reports[t].not_foldable.emplace_back(
                    fmt::format("{} [{}]: {}",
                                templates[t].text,
                                describe_operands(templates[t], c.operands),
                                c.diagnostic));
                break;
            }
        }
        reports[t].fold_errors = errors.size();
        // Spread the sampled error cases over the whole set
        const auto count{
            templates[t].fold_errors_panic ? std::min(opts.panic_samples, errors.size()) : 0UZ};
        for (usize s{0}; s < count; ++s) {
            samples.emplace_back<error_sample>({t, errors[s * errors.size() / count]});
        }
    }
    if (!opts.run || !opts.triple.empty()) { return reports; }

    // What each template needs from compiler_rt, probed at most once
    std::vector<stdx::option<std::vector<std::string>>> probed(templates.size());
    const auto                                          needs_of = [&](usize                  t,
                              const classified_case& sample) -> const std::vector<std::string>& {
        if (!probed[t]) {
            const phase_timer timer{fmt::format("probe compiler_rt for {}", templates[t].text)};
            probed[t] = template_needs(templates[t], sample);
        }
        return *probed[t];
    };

    // Batch templates into programs; a failing template is located on its own and dropped
    std::vector<bool>  linked(templates.size(), false);
    std::vector<usize> pending;
    for (usize t{0}; t < templates.size(); ++t) {
        reports[t].compared = foldable[t].size();
        if (!foldable[t].empty()) { pending.emplace_back(t); }
    }
    while (!pending.empty()) {
        std::vector<usize> batch;
        usize              total{0};
        while (!pending.empty() && batch.size() < max_cases_per_program &&
               (batch.empty() || total + foldable[pending.front()].size() <= max_batch_cases)) {
            total += foldable[pending.front()].size();
            batch.emplace_back(pending.front());
            pending.erase(pending.begin());
        }
        while (!batch.empty()) {
            std::vector<const expr_template*>             tmpls;
            std::vector<gsl::span<const classified_case>> spans;
            for (const auto t : batch) {
                tmpls.emplace_back(&templates[t]);
                spans.emplace_back(foldable[t]);
            }
            std::vector<std::string> missing;
            const auto               bad{[&] {
                const phase_timer timer{fmt::format("run batch of {} templates", batch.size())};
                return run_batch(tmpls, spans, missing);
            }()};
            if (missing.empty()) {
                for (const auto t : batch) { linked[t] = true; }
            }
            // Set aside what can't link yet, then rerun the rest of the batch
            if (!missing.empty()) {
                if (get_env("GHOTI_DIFF_TRACE")) {
                    fmt::println(stderr, "[diff] batch is missing {}", fmt::join(missing, ", "));
                }
                const auto before{batch.size()};
                std::erase_if(batch, [&](usize t) {
                    const auto& needs{needs_of(t, foldable[t].front())};
                    if (needs.empty()) { return false; }
                    reports[t].waiting_on = needs;
                    return true;
                });
                if (batch.size() < before) { continue; }
                // No single probe explains it: narrow down one template at a time
                if (batch.size() == 1) {
                    reports[batch.front()].waiting_on = std::move(missing);
                    break;
                }
                pending.insert(pending.begin(), batch.begin() + 1, batch.end());
                batch.resize(1);
                continue;
            }
            if (!bad) { break; }
            // A panic can't say which template, so each is checked on its own
            if (*bad >= batch.size()) {
                const phase_timer timer{
                    fmt::format("locate a panic among {} templates", batch.size())};
                for (const auto t : batch) {
                    if (auto found{locate_mismatch(templates[t], foldable[t])}) {
                        reports[t].mismatches.emplace_back(std::move(*found));
                    }
                }
                break;
            }
            const auto t{batch[*bad]};
            if (auto found{locate_mismatch(templates[t], foldable[t])}) {
                reports[t].mismatches.emplace_back(std::move(*found));
            }
            batch.erase(batch.begin(), batch.begin() + static_cast<idiff>(*bad) + 1);
        }
    }

    std::erase_if(samples, [&](const error_sample& sample) {
        auto& rep{reports[sample.template_index]};
        // A template that already linked needs nothing more from compiler_rt
        if (rep.waiting_on.empty() && !linked[sample.template_index]) {
            rep.waiting_on = needs_of(sample.template_index, sample.sample);
        }
        return !rep.waiting_on.empty();
    });
    {
        const phase_timer timer{fmt::format("check {} error samples", samples.size())};
        check_errors(templates, samples, reports);
    }
    return reports;
}

auto check(const expr_template& tmpl, const options& opts) -> report {
    return check_all({tmpl}, opts).front();
}

auto describe(const expr_template& tmpl, const report& rep) -> std::string {
    std::string out{fmt::format("{} -> {}: {} compared, {} fold errors, {} panics checked\n",
                                tmpl.text,
                                tmpl.result.name,
                                rep.compared,
                                rep.fold_errors,
                                rep.panics_checked)};
    for (const auto& m : rep.mismatches) { out += fmt::format("  MISMATCH {}\n", m.description); }
    for (const auto& n : rep.not_foldable) { out += fmt::format("  NOT FOLDABLE {}\n", n); }
    if (!rep.waiting_on.empty()) {
        out += fmt::format("  runtime needs compiler_rt: {}\n", fmt::join(rep.waiting_on, ", "));
    }
    return out;
}

} // namespace ghoti::tests::differential
