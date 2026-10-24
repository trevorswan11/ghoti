#include "helpers/fuzz.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <mutex>
#include <random>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/base.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "driver/clap/error.hh"
#include "support/env.hh"
#include "support/path_utils.hh"
#include "support/string_utils.hh"
#include "support/subprocess.hh"

namespace ghoti::tests::fuzz {

namespace {

constexpr usize                     MIN_SEED_LENGTH{40};
constexpr usize                     MAX_SPAN{11};
constexpr usize                     SIGNATURE_LENGTH{160};
constexpr usize                     EXIT_LINE_LENGTH{100};
constexpr usize                     REPORTED_PER_GROUP{4};
constexpr std::chrono::milliseconds CASE_TIMEOUT{std::chrono::seconds{60}};
constexpr auto COMPILATION_FAILED{static_cast<u32>(clap::error::COMPILATION_FAILED)};

// Tokens a replacement mutation picks from
constexpr std::array REPLACEMENTS{
    "(",
    ")",
    "{",
    "}",
    "[",
    "]",
    ";",
    ",",
    ".",
    ":",
    "=",
    "..",
    "^",
    "&",
    "?",
    "!",
    "fn",
    "struct",
    "union",
    "enum",
    "match",
    "if",
    "else",
    "return",
    "break",
    "continue",
    "loop",
    "for",
    "while",
    "unroll",
    "defer",
    "errdefer",
    "const",
    "let",
    "let mut",
    "comptime",
    "pub",
    "type",
    "void",
    "noreturn",
    "i32",
    "u8",
    "bool",
    "true",
    "undefined",
    "0",
    "1",
    "-1",
    R"("s")",
    "@as",
    "@sizeOf",
    "@typeInfo",
    "@This",
    "@compileError",
    "mut",
    "dyn",
    "impl",
    "interface",
    "self",
    "...",
    "_",
    "=>",
    "|x|",
    "null",
    "test",
    "import",
    "extern",
    "export",
    "@\"\U0001F600\"",
    R"(@"caf\u{E9}")",
    "\"\u4F60\u597D\\u{1F600}\"",
    R"("\xFF")",
    "'\u00E9'",
    "'\U0001F600'",
    R"('\u{10FFFF}')",
    "'ab'",
    R"("\u{D800}")",
};

// Non-ASCII raw identifiers a rename mutation picks from
constexpr std::array RAW_IDENTIFIERS{
    "@\"\U0001F600\"",
    "@\"\u5185\u90E8\"",
    R"(@"e\u{301}")",
    "@\"\U0001F468\u200D\U0001F469\u200D\U0001F467\"",
};

// The markers a compiler crash leaves in its output
constexpr std::array<std::string_view, 8> CRASH_LITERALS{
    "Assertion failed",
    "unreachable reached",
    "LLVM ERROR",
    "Broken module",
    "Stack dump",
    "libc++abi",
    "Instruction does not dominate",
    "Referring to an instruction in another function",
};

[[nodiscard]] auto pick(std::mt19937_64& rng, usize count) -> usize {
    return std::uniform_int_distribution<usize>{0, count - 1}(rng);
}

[[nodiscard]] auto run_end(std::string_view text, usize from, auto&& keep) -> usize {
    while (from < text.size() && keep(text[from])) { ++from; }
    return from;
}

// The end of a string literal closed on its own line
[[nodiscard]] auto string_literal_end(std::string_view source, usize quote) -> stdx::option<usize> {
    for (auto at{quote + 1}; at < source.size(); ++at) {
        if (source[at] == '"') { return at + 1; }
        if (source[at] == '\n') { return stdx::none; }
        if (source[at] == '\\') { ++at; }
    }
    return stdx::none;
}

// The end of the token starting at `at`; an unterminated quote is a token of its own
[[nodiscard]] auto token_end(std::string_view source, usize at) -> usize {
    const char c{source[at]};
    if (string_utils::is_space(c)) { return run_end(source, at, string_utils::is_space); }
    if (string_utils::is_identifier_start(c)) {
        return run_end(source, at + 1, string_utils::is_word);
    }
    if (string_utils::is_digit(c)) { return run_end(source, at, string_utils::is_digit); }
    if (c == '"') {
        if (const auto end{string_literal_end(source, at)}) { return *end; }
    }
    return std::min(source.size(), at + string_utils::utf8_width(c));
}

[[nodiscard]] auto join(auto&& tokens) -> std::string {
    std::string out;
    for (const auto& token : tokens) { out += token; }
    return out;
}

[[nodiscard]] auto line_end(std::string_view text, usize from) -> usize {
    const auto end{text.find_first_of("\r\n", from)};
    return end == std::string_view::npos ? text.size() : end;
}

[[nodiscard]] auto earliest(stdx::option<usize> a, usize b) -> stdx::option<usize> {
    return a && *a < b ? a : stdx::option<usize>{b};
}

// `panic:` not preceded by `@`, so a program's own `@panic:` text doesn't count
[[nodiscard]] auto find_panic(std::string_view output) -> stdx::option<usize> {
    for (auto at{output.find("panic:")}; at != std::string_view::npos;
         at = output.find("panic:", at + 1)) {
        if (at == 0 || output[at - 1] != '@') { return at; }
    }
    return stdx::none;
}

// `<digits>:<digits>` at `at`
[[nodiscard]] auto is_line_column(std::string_view text, usize at) -> bool {
    const auto colon{run_end(text, at, string_utils::is_digit)};
    if (colon == at || colon >= text.size() || text[colon] != ':') { return false; }
    return run_end(text, colon + 1, string_utils::is_digit) > colon + 1;
}

// An assertion's Windows source location: `): C:\...\file.cc:12:5`
[[nodiscard]] auto find_source_location(std::string_view output) -> stdx::option<usize> {
    for (auto at{output.find("): ")}; at != std::string_view::npos;
         at = output.find("): ", at + 1)) {
        const auto drive{at + 3};
        if (drive + 1 >= output.size() || std::isalpha(static_cast<u8>(output[drive])) == 0 ||
            output[drive + 1] != ':') {
            continue;
        }
        const auto line{
            stdx::string::substr(output, drive + 2, line_end(output, drive + 2) - (drive + 2))};
        for (const std::string_view ext : {".cc:", ".hh:"}) {
            for (auto e{line.find(ext)}; e != std::string_view::npos; e = line.find(ext, e + 1)) {
                if (is_line_column(line, e + ext.size())) { return at; }
            }
        }
    }
    return stdx::none;
}

// An LLVM verifier type mismatch: `must have ... type (`
[[nodiscard]] auto find_type_mismatch(std::string_view output) -> stdx::option<usize> {
    constexpr std::string_view lead{"must have "};
    for (auto at{output.find(lead)}; at != std::string_view::npos; at = output.find(lead, at + 1)) {
        const auto from{at + lead.size()};
        if (stdx::string::substr(output, from, line_end(output, from) - from).find(" type (") !=
            std::string_view::npos) {
            return at;
        }
    }
    return stdx::none;
}

[[nodiscard]] auto strip_ansi(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (usize i{0}; i < text.size(); ++i) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            auto end{i + 2};
            while (end < text.size() && (string_utils::is_digit(text[end]) || text[end] == ';')) {
                ++end;
            }
            if (end < text.size() && text[end] == 'm') {
                i = end;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

[[nodiscard]] auto describe_exit(const stdx::option<u32>& exit_code) -> std::string {
    if (!exit_code) { return "unspawned"; }
    if (*exit_code == spawn_child_timeout_exit_code) { return "TIMEOUT"; }
    return fmt::format("{}", *exit_code);
}

auto try_remove(const std::filesystem::path& path) -> void {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

} // namespace

auto raw_strings(std::string_view cc_source) -> std::vector<std::string_view> {
    std::vector<std::string_view> bodies;
    usize                         at{0};
    while ((at = cc_source.find("R\"", at)) != std::string_view::npos) {
        auto open{at + 2};
        while (open < cc_source.size() && string_utils::is_word(cc_source[open])) { ++open; }
        if (open >= cc_source.size() || cc_source[open] != '(') {
            ++at;
            continue;
        }
        const auto tag{stdx::string::substr(cc_source, at + 2, open - (at + 2))};
        const auto close{cc_source.find(fmt::format("){}\"", tag), open + 1)};
        if (close == std::string_view::npos) { break; }
        bodies.emplace_back(stdx::string::substr(cc_source, open + 1, close - (open + 1)));
        at = close + tag.size() + 2;
    }
    return bodies;
}

auto seed_programs(const std::filesystem::path& repo_root) -> std::vector<fuzz_case> {
    const auto collect{[](const std::filesystem::path& dir, std::string_view ext, bool recursive) {
        std::vector<std::filesystem::path> files;
        std::error_code                    ec;
        const auto                         keep{[&](const std::filesystem::directory_entry& e) {
            if (e.is_regular_file() && e.path().extension() == ext) {
                files.emplace_back(e.path());
            }
        }};
        if (recursive) {
            for (const auto& e : std::filesystem::recursive_directory_iterator{dir, ec}) {
                keep(e);
            }
        } else {
            for (const auto& e : std::filesystem::directory_iterator{dir, ec}) { keep(e); }
        }
        std::ranges::sort(files);
        return files;
    }};

    std::vector<fuzz_case> programs;
    const auto             tests_dir{repo_root / "lib" / "compiler" / "tests"};
    for (const auto& file : collect(tests_dir, ".cc", true)) {
        auto stem{std::filesystem::relative(file, tests_dir).replace_extension().generic_string()};
        std::ranges::replace(stem, '/', '.');

        const auto text{path_utils::read_file(file)};
        const auto bodies{raw_strings(text)};
        for (usize i{0}; i < bodies.size(); ++i) {
            const auto body{bodies[i]};
            if (body.size() < MIN_SEED_LENGTH || (body.find("fn") == std::string_view::npos &&
                                                  body.find("struct") == std::string_view::npos)) {
                continue;
            }

            // Formatter-style tests double their braces for fmt
            std::string source{body};
            if (source.contains("{{") && source.contains("}}")) {
                std::string undoubled;
                for (usize c{0}; c < source.size(); ++c) {
                    undoubled += source[c];
                    if ((source[c] == '{' || source[c] == '}') && c + 1 < source.size() &&
                        source[c + 1] == source[c]) {
                        ++c;
                    }
                }
                source = std::move(undoubled);
            }
            programs.emplace_back<fuzz_case>({
                .name   = fmt::format("{}_{}", stem, i),
                .source = std::move(source),
            });
        }
    }
    for (const auto& file : collect(repo_root / "examples", ".gh", false)) {
        programs.emplace_back<fuzz_case>({
            .name   = "ex_" + file.stem().string(),
            .source = path_utils::read_file(file),
        });
    }
    return programs;
}

auto tokenize(std::string_view source) -> std::vector<std::string_view> {
    std::vector<std::string_view> tokens;
    usize                         at{0};
    while (at < source.size()) {
        const auto end{token_end(source, at)};
        tokens.emplace_back(stdx::string::substr(source, at, end - at));
        at = end;
    }
    return tokens;
}

auto mutate(std::string_view source, std::mt19937_64& rng) -> std::string {
    std::vector<std::string> tokens;
    for (const auto token : tokenize(source)) { tokens.emplace_back(token); }
    if (tokens.size() < 4) { return std::string{source}; }

    enum mutation_kind : usize {
        TRUNCATE,
        DELETE_A_TOKEN,
        DUPLICATE_A_TOKEN,
        SWAP_WITH_TOKEN,
        REPLACE_WITH_TOKEN,
        DELETE_A_SPAN,
        RENAME_AN_IDENT,
    };

    const auto kind{pick(rng, 8)};
    const auto i{pick(rng, tokens.size())};
    const auto begin{tokens.begin()};
    switch (kind) {
    case TRUNCATE:       tokens.resize(i); return join(tokens);
    case DELETE_A_TOKEN: tokens.erase(begin + static_cast<isize>(i)); return join(tokens);
    case DUPLICATE_A_TOKEN:
        tokens.insert(begin + static_cast<isize>(i), tokens[i]);
        return join(tokens);
    case SWAP_WITH_TOKEN:
        std::swap(tokens[i], tokens[pick(rng, tokens.size())]);
        return join(tokens);
    case REPLACE_WITH_TOKEN:
        tokens[i] = fmt::format(" {} ", REPLACEMENTS[pick(rng, REPLACEMENTS.size())]);
        return join(tokens);
    case DELETE_A_SPAN: {
        const auto end{std::min(tokens.size(), i + 1 + pick(rng, MAX_SPAN))};
        tokens.erase(begin + static_cast<isize>(i), begin + static_cast<isize>(end));
        return join(tokens);
    }
    case RENAME_AN_IDENT: {
        ankerl::unordered_dense::set<std::string_view> names;
        for (const auto& token : tokens) {
            if (string_utils::is_ascii_identifier(token)) { names.insert(token); }
        }
        if (names.empty()) { break; }
        const std::string name{
            *std::next(names.begin(), static_cast<isize>(pick(rng, names.size())))};
        const std::string raw{RAW_IDENTIFIERS[pick(rng, RAW_IDENTIFIERS.size())]};
        for (auto& token : tokens) {
            if (token == name) { token = raw; }
        }
        return join(tokens);
    }
    default: break;
    }

    // Splice a span from elsewhere; a rename with no identifiers lands here too
    const auto                     from{pick(rng, tokens.size())};
    const auto                     to{std::min(tokens.size(), from + 1 + pick(rng, MAX_SPAN))};
    const std::vector<std::string> span{begin + static_cast<isize>(from),
                                        begin + static_cast<isize>(to)};
    tokens.insert(begin + static_cast<isize>(i), span.begin(), span.end());
    return join(tokens);
}

auto mutated_cases(const std::vector<fuzz_case>& programs, u64 seed, usize per_program)
    -> std::vector<fuzz_case> {
    std::mt19937_64        rng{seed};
    std::vector<fuzz_case> cases;
    cases.reserve(programs.size() * per_program);
    for (const auto& program : programs) {
        for (usize k{0}; k < per_program; ++k) {
            cases.emplace_back<fuzz_case>({
                .name   = fmt::format("s{}_{}_{}", seed, program.name, k),
                .source = mutate(program.source, rng),
            });
        }
    }
    return cases;
}

auto find_crash_marker(std::string_view output) -> stdx::option<usize> {
    stdx::option<usize> first;
    for (const auto literal : CRASH_LITERALS) {
        if (const auto at{output.find(literal)}; at != std::string_view::npos) {
            first = earliest(first, at);
        }
    }
    for (const auto found :
         {find_panic(output), find_source_location(output), find_type_mismatch(output)}) {
        if (found) { first = earliest(first, *found); }
    }
    return first;
}

auto is_compiler_bug(const outcome& run) -> bool {
    if (!run.exit_code || (*run.exit_code != 0 && *run.exit_code != COMPILATION_FAILED)) {
        return true;
    }
    if (find_crash_marker(run.output)) { return true; }

    // A compile error exit must come with a reported error
    std::string lowered{run.output};
    std::ranges::transform(lowered, lowered.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<u8>(c)));
    });
    return *run.exit_code == COMPILATION_FAILED && !lowered.contains("error");
}

auto signature(const outcome& run) -> std::string {
    const std::string_view output{run.output};
    if (const auto marker{find_crash_marker(output)}) {
        const auto line{stdx::string::substr(output, *marker, line_end(output, *marker) - *marker)};
        std::string blanked;
        for (usize i{0}; i < line.size(); ++i) {
            if (!string_utils::is_digit(line[i])) {
                blanked += line[i];
                continue;
            }
            blanked += 'N';
            while (i + 1 < line.size() && string_utils::is_digit(line[i + 1])) { ++i; }
        }
        return blanked.substr(0, SIGNATURE_LENGTH);
    }

    // The last line of output, if any
    auto trimmed{output};
    while (!trimmed.empty() && string_utils::is_space(trimmed.back())) { trimmed.remove_suffix(1); }
    const auto last_line_start{trimmed.find_last_of("\r\n")};
    const auto last_line{last_line_start == std::string_view::npos
                             ? trimmed
                             : stdx::string::substr(trimmed, last_line_start + 1)};
    return fmt::format("exit={} {}",
                       describe_exit(run.exit_code),
                       stdx::string::substr(last_line, 0, EXIT_LINE_LENGTH));
}

auto harness::from_env() -> harness {
    harness    h;
    const auto ghoti{get_env("GHOTI")};
    h.ghoti = ghoti ? std::filesystem::path{*ghoti} : ghoti_binary_path();
    const auto cases{get_env("GHOTI_FUZZ_CASES")};
    h.cases_dir = cases ? std::filesystem::path{*cases}
                        : std::filesystem::temp_directory_path() / "ghoti-fuzz";
    std::filesystem::create_directories(h.cases_dir);
    return h;
}

auto harness::case_path(std::string_view name) const -> std::filesystem::path {
    return cases_dir / fmt::format("{}.gh", name);
}

auto harness::run_one(const fuzz_case& c) const -> outcome {
    const auto source_path{case_path(c.name)};
    const auto object_path{cases_dir / fmt::format("{}.o", c.name)};
    const auto log_path{cases_dir / fmt::format("{}.log", c.name)};
    {
        std::ofstream out{source_path, std::ios::binary};
        out << c.source;
    }

    std::vector<std::string> args{ghoti.string(), command, source_path.string()};
    if (command == "build-obj" || command == "build-exe") {
        args.emplace_back("-o");
        args.emplace_back(object_path.string());
    }
    outcome run{
        .name      = c.name,
        .exit_code = spawn_child_logged(mock_argv{std::move(args)}, log_path, CASE_TIMEOUT),
        .output    = strip_ansi(path_utils::read_file(log_path)),
    };

    try_remove(object_path);
    try_remove(log_path);
    if (!is_compiler_bug(run)) { try_remove(source_path); }
    return run;
}

auto harness::run_all(const std::vector<fuzz_case>& cases) const -> std::vector<outcome> {
    std::vector<outcome> failures;
    std::mutex           failures_mutex;
    std::atomic<usize>   next{0};
    {
        std::vector<std::jthread> workers;
        for (usize w{0}; w < std::max(jobs, 1UZ); ++w) {
            workers.emplace_back([&] {
                for (auto i{next++}; i < cases.size(); i = next++) {
                    auto run{run_one(cases[i])};
                    if (!is_compiler_bug(run)) { continue; }
                    const std::scoped_lock lock{failures_mutex};
                    failures.emplace_back(std::move(run));
                }
            });
        }
    }
    std::ranges::sort(failures, {}, &outcome::name);
    return failures;
}

auto group_into(failure_groups& groups, std::vector<outcome> failures) -> void {
    for (auto& failure : failures) {
        auto key{signature(failure)};
        groups[std::move(key)].emplace_back(std::move(failure));
    }
}

auto report(const harness& h, const failure_groups& groups) -> std::string {
    std::vector<gsl::not_null<const failure_groups::value_type*>> ordered;
    for (const auto& group : groups) { ordered.emplace_back(&group); }
    std::ranges::stable_sort(
        ordered, std::greater{}, [](const auto g) { return g->second.size(); });

    std::string out;
    for (const auto group : ordered) {
        const auto& [sig, items]{*group};
        fmt::format_to(std::back_inserter(out), "#### {} x {}\n", items.size(), sig);
        for (const auto& item : items | std::views::take(REPORTED_PER_GROUP)) {
            fmt::format_to(std::back_inserter(out),
                           "    {} {}\n",
                           h.case_path(item.name).string(),
                           describe_exit(item.exit_code));
        }
    }
    return out;
}

} // namespace ghoti::tests::fuzz
