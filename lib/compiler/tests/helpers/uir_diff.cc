#include "helpers/uir_diff.hh"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/codegen/target.hh"
#include "compiler/gir/dumper.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/module/source_loader.hh"
#include "compiler/sema/analyzer.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"
#include "fmt/base.h"
#include "helpers/gir_effects.hh"
#include "helpers/gir_normalize.hh"
#include "stdx/hash.hh"
#include "support/diagnostic.hh"
#include "support/env.hh"

namespace ghoti::tests::helpers {

namespace {

// Whether an immutable `let` is initialized by a call, which the old emitter folds
[[nodiscard]] auto has_folded_let(gsl::span<const uir::lowered_module> modules) -> bool {
    for (const auto& item : modules) {
        const auto& m{item.result.uir};
        for (u32 i{0}; i < m.instruction_count(); ++i) {
            const uir::inst_id inst{i};
            const auto         is_call{[&](stdx::option<uir::inst_id> i) {
                return i &&
                       (m.tag_of(*i) == uir::tag::call || m.tag_of(*i) == uir::tag::field_call);
            }};

            // A local `const` whose body yields a call is one too
            if (m.tag_of(inst) == uir::tag::const_local) {
                uir::record_reader rec{m, m.data_of(inst).pl.extra};
                rec.skip_str().skip_ref();
                const auto body{rec.body_field()};
                if (!body.empty() && m.tag_of(body.back()) == uir::tag::yield &&
                    is_call(uir::as_inst(m.data_of(body.back()).un.operand))) {
                    return true;
                }

                continue;
            }

            if (m.tag_of(inst) != uir::tag::let_value) { continue; }
            uir::record_reader rec{m, m.data_of(inst).pl.extra};
            rec.skip_str().skip_ref();
            const auto init{rec.ref_field().and_then(uir::as_inst)};
            if (init &&
                (m.tag_of(*init) == uir::tag::call || m.tag_of(*init) == uir::tag::field_call)) {
                return true;
            }
        }
    }
    return false;
}

// Whether UIR calls a function the old dump defines but never calls
[[nodiscard]] auto calls_folded_function(std::string_view uir_text, std::string_view legacy_text)
    -> bool {
    for (const auto prefix : {std::string_view{"call @"}, std::string_view{"call \""}}) {
        for (usize at{uir_text.find(prefix)}; at != std::string_view::npos;
             at = uir_text.find(prefix, at + 1)) {
            const auto start{at + prefix.size()};
            const auto end{uir_text.find_first_of("\"(", start)};
            if (end == std::string_view::npos) { break; }
            const auto name{stdx::string::substr(uir_text, start, end - start)};
            const bool called{
                legacy_text.find(fmt::format("call @{}(", name)) != std::string_view::npos ||
                legacy_text.find(fmt::format("call \"{}\"(", name)) != std::string_view::npos};
            const bool defined{legacy_text.find(fmt::format("fn {}(", name)) !=
                               std::string_view::npos};
            if (!called && defined) { return true; }
        }
    }

    return false;
}

// Whether UIR calls a function with only constant arguments that the old dump never calls
[[nodiscard]] auto has_folded_call(std::string_view uir_text, std::string_view legacy_text)
    -> bool {
    for (usize at{uir_text.find("call @")}; at != std::string_view::npos;
         at = uir_text.find("call @", at + 1)) {
        const auto open{uir_text.find('(', at)};
        const auto close{uir_text.find(')', open)};
        if (open == std::string_view::npos || close == std::string_view::npos) { break; }
        const auto args{stdx::string::substr(uir_text, open + 1, close - open - 1)};
        if (args.find('%') != std::string_view::npos ||
            args.find("param.") != std::string_view::npos) {
            continue;
        }
        // The same callee may still be called elsewhere with runtime arguments
        if (legacy_text.find(stdx::string::substr(uir_text, at, close - at + 1)) ==
            std::string_view::npos) {
            return true;
        }
    }

    return calls_folded_function(uir_text, legacy_text);
}

[[nodiscard]] auto without_unused_on_demand(std::string_view legacy_text, std::string_view uir_text)
    -> std::string;

// How many times `text` names `@symbol`, as a whole name
[[nodiscard]] auto references(std::string_view text, std::string_view symbol) -> usize {
    const auto ident{[](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '#';
    }};
    const auto needle{fmt::format("@{}", symbol)};
    usize      n{0};
    for (auto at{text.find(needle)}; at != std::string_view::npos; at = text.find(needle, at + 1)) {
        const auto after{at + needle.size()};
        if (after < text.size() && ident(text[after])) { continue; }
        ++n;
    }
    return n;
}

// The old dump without the globals and types UIR never emitted and nothing it keeps names
[[nodiscard]] auto without_unused_items(std::string_view legacy_text, std::string_view uir_text)
    -> std::string {
    std::string kept;
    for (usize at{0}; at < legacy_text.size();) {
        const auto line_end{legacy_text.find('\n', at)};
        const auto end{line_end == std::string_view::npos ? legacy_text.size() : line_end + 1};
        const auto line{stdx::string::substr(legacy_text, at, end - at)};
        at = end;
        const bool global{line.starts_with("const ")};
        if (!global && !line.starts_with("type ")) {
            kept += line;
            continue;
        }

        const auto name_start{line.find(' ') + 1};
        const auto name{stdx::string::substr(
            line, name_start, line.find_first_of(global ? ":" : " =", name_start) - name_start)};
        const bool in_uir{uir_text.find(line) != std::string_view::npos};
        if (in_uir || (global && references(legacy_text, name) != 0)) { kept += line; }
    }
    return kept;
}

// The old dump pruned until nothing it keeps is called only from what it dropped
[[nodiscard]] auto without_all_unused(std::string_view legacy_text, std::string_view uir_text)
    -> std::string {
    std::string kept{legacy_text};
    for (auto next{without_unused_on_demand(kept, uir_text)}; next != kept;
         next = without_unused_on_demand(kept, uir_text)) {
        kept = std::move(next);
    }
    return without_unused_items(kept, uir_text);
}

// Every `fn <prefix><digits>.<member>(` header of a dump, as its qualifier and member
[[nodiscard]] auto numbered_members(std::string_view text, std::string_view prefix)
    -> std::vector<std::pair<std::string, std::string>> {
    std::vector<std::pair<std::string, std::string>> out;
    const auto                                       header{fmt::format("fn {}", prefix)};
    for (usize at{text.find(header)}; at != std::string_view::npos;
         at = text.find(header, at + 1)) {
        if (at != 0 && text[at - 1] != '\n') { continue; }
        auto end{at + header.size()};
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; }
        const auto open{text.find('(', end)};
        if (end == at + header.size() || end >= text.size() || text[end] != '.' ||
            open == std::string_view::npos) {
            continue;
        }
        out.emplace_back(stdx::string::substr(text, at + 3, end - at - 3),
                         stdx::string::substr(text, end + 1, open - end - 1));
    }
    return out;
}

// The old emitter names a no-argument ctor instance `tcN` after a symbol table slot, so the one
// UIR qualifier whose members the old dump lacks takes that name
[[nodiscard]] auto with_legacy_ctor_names(std::string_view legacy_text, std::string uir_text)
    -> std::string {
    for (const auto& [legacy_name, member] : numbered_members(legacy_text, "tc")) {
        if (uir_text.find(fmt::format("fn {}.{}(", legacy_name, member)) != std::string::npos) {
            continue;
        }

        // UIR's qualifiers of a member of this name that the old dump doesn't have
        const auto               suffix{fmt::format(".{}(", member)};
        std::vector<std::string> candidates;
        for (usize at{uir_text.find(suffix)}; at != std::string::npos;
             at = uir_text.find(suffix, at + 1)) {
            const auto line{uir_text.rfind('\n', at)};
            const auto start{line == std::string::npos ? 0 : line + 1};
            if (uir_text.compare(start, 3, "fn ") != 0) { continue; }
            auto qualifier{uir_text.substr(start + 3, at - start - 3)};
            if (qualifier.find_first_of(". ") != std::string::npos ||
                legacy_text.find(fmt::format("fn {}{}", qualifier, suffix)) !=
                    std::string_view::npos) {
                continue;
            }
            candidates.emplace_back(std::move(qualifier));
        }
        if (candidates.size() != 1) { continue; }

        for (const auto& [from, to] : {std::pair{fmt::format("fn {}.", candidates.front()),
                                                 fmt::format("fn {}.", legacy_name)},
                                       std::pair{fmt::format("@{}.", candidates.front()),
                                                 fmt::format("@{}.", legacy_name)}}) {
            for (auto at{uir_text.find(from)}; at != std::string::npos;
                 at = uir_text.find(from, at + to.size())) {
                uir_text.replace(at, from.size(), to);
            }
        }
    }
    return uir_text;
}

// The old dump without the impl methods and type-ctor members UIR analyzes on demand and never used
[[nodiscard]] auto without_unused_on_demand(std::string_view legacy_text, std::string_view uir_text)
    -> std::string {
    std::string                                    kept;
    std::map<std::string_view, usize, std::less<>> kept_copies;
    for (usize at{0}; at < legacy_text.size();) {
        // A test function ends the one before it too
        const auto next{
            std::min(legacy_text.find("\nfn ", at + 1), legacy_text.find("\ntest \"", at + 1))};
        const auto end{next == std::string_view::npos ? legacy_text.size() : next + 1};
        const auto function{stdx::string::substr(legacy_text, at, end - at)};
        const auto header_end{function.find('(')};
        const auto name{stdx::string::substr(function, 0, header_end)};

        // A type-ctor instance's member is `Ctor__args.member`, a generic's is `name__args`
        const auto mangled{name.find("__")};
        const bool on_demand{function.starts_with("fn pimpl#") ||
                             mangled != std::string_view::npos};
        // A function nothing calls or takes the address of is one the old emitter emits eagerly
        const bool absent{uir_text.find(name) == std::string_view::npos};
        // A function calling another of the same name doesn't count as using it
        const bool referenced{name.size() > 3 && references(legacy_text, name.substr(3)) >
                                                     references(function, name.substr(3))};
        // The globals and types before the first function are left to `without_unused_items`
        const bool unused{function.starts_with("fn ") && absent && (on_demand || !referenced)};
        // Instances whose names normalize alike count as unused when UIR has fewer of them
        const auto header{fmt::format("\n{}(", name)};
        const auto count{[&](std::string_view text) {
            usize n{0};
            for (auto found{text.find(header)}; found != std::string_view::npos;
                 found = text.find(header, found + 1)) {
                ++n;
            }
            return n;
        }};
        // and no more copies of one body are kept than UIR has
        const auto copies{[&] {
            usize n{0};
            for (auto found{uir_text.find(function)}; found != std::string_view::npos;
                 found = uir_text.find(function, found + 1)) {
                ++n;
            }
            return n;
        }};
        const bool duplicate{on_demand && count(uir_text) < count(legacy_text) &&
                             kept_copies[function] >= copies()};
        if (!unused && !duplicate) {
            kept += function;
            ++kept_copies[function];
        }
        at = end;
    }

    return kept;
}

// A normalized dump split into its non-function lines and its functions by header name
struct split_dump {
    std::string                                     items;
    std::map<std::string, std::string, std::less<>> functions;
};

[[nodiscard]] auto split_functions(std::string_view text) -> split_dump {
    split_dump out;
    usize      at{0};
    while (at < text.size()) {
        const bool starts{text.substr(at).starts_with("fn ") ||
                          text.substr(at).starts_with("test \"")};
        if (!starts) {
            const auto line_end{text.find('\n', at)};
            const auto end{line_end == std::string_view::npos ? text.size() : line_end + 1};
            out.items += text.substr(at, end - at);
            at = end;
            continue;
        }

        const auto next_fn{text.find("\nfn ", at + 1)};
        const auto next_test{text.find("\ntest \"", at + 1)};
        const auto next{std::min(next_fn, next_test)};
        const auto end{next == std::string_view::npos ? text.size() : next + 1};
        const auto function{text.substr(at, end - at)};
        const auto name{function.substr(0, function.find_first_of("(\n"))};
        out.functions[std::string{name}] += function;
        at = end;
    }

    return out;
}

// Whether UIR calls the function `fn name` somewhere, and only ever with constant arguments
[[nodiscard]] auto only_folded_calls(std::string_view name, std::string_view uir_text) -> bool {
    if (!name.starts_with("fn ")) { return false; }
    const auto call{fmt::format("call @{}(", name.substr(3))};
    bool       called{false};
    for (auto at{uir_text.find(call)}; at != std::string_view::npos;
         at = uir_text.find(call, at + 1)) {
        const auto open{at + call.size()};
        const auto close{uir_text.find(')', open)};
        if (close == std::string_view::npos) { return false; }
        const auto args{stdx::string::substr(uir_text, open, close - open)};
        if (args.find('%') != std::string_view::npos ||
            args.find("param.") != std::string_view::npos) {
            return false;
        }
        called = true;
    }

    return called;
}

// Whether every difference is a call UIR makes where the old emitter folded it, one function at
// a time, so a fold in one function can't excuse a difference in another
[[nodiscard]] auto only_fold_placement(std::string_view uir_text,
                                       std::string_view legacy_text,
                                       bool folded_let) -> stdx::option<uir_diff_status> {
    // `GHOTI_UIR_DIFF_WHY` names what kept a program from counting as fold placement
    const auto rejected{[](std::string_view why, std::string_view name) {
        if (get_env("GHOTI_UIR_DIFF_WHY")) { fmt::print(stderr, "uir-diff: {} {}\n", why, name); }
        return stdx::option<uir_diff_status>{};
    }};
    const auto uir{split_functions(uir_text)};
    const auto legacy{split_functions(without_all_unused(legacy_text, uir_text))};
    if (uir.items != legacy.items) { return rejected("types or globals differ", ""); }
    bool folded{false};
    for (const auto& [name, body] : legacy.functions) {
        const auto found{uir.functions.find(name)};
        if (found == uir.functions.end()) { return rejected("UIR lacks", name); }
        if (found->second == body || same_effects(found->second, body)) { continue; }
        folded = true;
        if (folded_let) { continue; }
        // UIR may fold what the old emitter calls with constant arguments, and the reverse
        if (!has_folded_call(found->second, body) &&
            !calls_folded_function(found->second, legacy_text) &&
            !has_folded_call(body, found->second)) {
            return rejected("differs", name);
        }
    }

    // A function only UIR has is one the old emitter folded every call to, or one nothing calls:
    // the panic handler only a check normalizing dropped as dead called, or a member analyzed
    // while a call was tried at compile time
    std::string shared{uir.items};
    for (const auto& [name, body] : uir.functions) {
        if (legacy.functions.contains(name)) { shared += body; }
    }
    const auto uncalled{[&](std::string_view name) {
        const auto symbol{name.substr(3)};
        const bool member{symbol.find('.') != std::string_view::npos};
        return (symbol == "panic_handler" || member) &&
               shared.find(fmt::format("@{}(", symbol)) == std::string::npos;
    }};

    // An internal function nothing calls or takes the address of, such as one only a
    // compile-time call used, which the old emitter never emits
    const auto unreferenced{[&](std::string_view name, std::string_view body) {
        const auto symbol{name.substr(3)};
        const auto header{body.substr(0, body.find('\n'))};
        return header.find("[internal]") != std::string_view::npos &&
               uir_text.find(fmt::format("@{}", symbol)) == std::string_view::npos &&
               uir_text.find(fmt::format("\"{}\"", symbol)) == std::string_view::npos;
    }};
    const bool extras_folded{std::ranges::all_of(uir.functions, [&](const auto& entry) {
        return legacy.functions.contains(entry.first) || uncalled(entry.first) ||
               unreferenced(entry.first, entry.second) || only_folded_calls(entry.first, uir_text);
    })};
    if (!extras_folded) { return rejected("UIR has a function the old dump lacks", ""); }
    const bool extra{std::ranges::any_of(
        uir.functions, [&](const auto& entry) { return !legacy.functions.contains(entry.first); })};
    return folded || extra ? uir_diff_status::FOLD_PLACEMENT : uir_diff_status::EVAL_ORDER;
}

[[nodiscard]] auto dump_with_flags(const gir::module_t& gir_mod) -> std::string {
    std::ostringstream out;
    gir::dumper        d{out};
    d.set_show_flags(true);
    d.dump(gir_mod);
    return out.str();
}

// The tests whose programs the old pipeline gets wrong in a way its GIR shows, by bug
constexpr std::array LEGACY_BUG_TESTS{
    // The old evaluator can't run a call returning an untyped module-level `const` as a float, so
    // `@intFromFloat(i32, as_float())` stays at runtime
    std::string_view{"A later global is typed on its own, not by the `return` that first reads it"},
    // A compile-time slice global read at runtime stores its backing array into the slice's slot
    std::string_view{"A compile-time slice used at runtime is backed by a stack array"},
    // An interface default method's parameter of an associated type is typed `type`
    std::string_view{"A `dyn` of an interface with a defaulted associated type"},
    // A guarded match whose arms are all untyped keeps its result in a `comptime_int` slot
    std::string_view{"Guards decide between arms at runtime"},
    // A module alias named after the primitive it names is listed as a global of that type
    std::string_view{"A folded const named after a primitive can read the primitive"},
    // A compile-time call skips the argument check a runtime one makes, so `Box(i32)` passes for a
    // `Box(comptime_int)` parameter
    std::string_view{"A later parameter's type may be a type-constructor call over an earlier "
                     "parameter"},
    // A type a `match comptime` yields is listed as a global constant of type `struct`, and its
    // local `const` value is built at runtime
    std::string_view{"A match arm may yield an `extern struct` type"},
    // The evaluator leaves a comparison with an infinite constant, like `@log(0.0)`, at runtime
    std::string_view{"math builtins reject integers, types, and results past the range"},
    // A product with a constant zero is folded with the call it multiplies, which never runs
    std::string_view{"An inferred cast's target never types its operand"},
    // A `let` holding a type through a labeled block's `break` isn't rejected
    std::string_view{"an inline aggregate type in value position has no runtime value"},
    // The address of an untyped constant points at a `comptime_int` slot
    std::string_view{"address of a scalar static member constant"},
    // The old emitter spills a `match` or `if` of untyped arms as `comptime_int`
    std::string_view{"Top-level runtime patterns stay lazy and in order"},
    std::string_view{"A pointer is truthy as an 'and' / 'or' operand"},
    // The old pipeline accepts a local `const` a runtime call initializes when it is an aggregate
    std::string_view{
        "A return inside a function literal runs none of the enclosing function's defers"},
    // The old pipeline accepts a local `const` slice of a runtime array
    std::string_view{"Slices match their length and strings their bytes"},
    std::string_view{"a subscript range cannot count down"},
    // `@as` narrows a runtime integer unchecked, where `@intCast` is the builtin for it
    std::string_view{"Aggregates past the by-pointer threshold cross the C ABI"},
    // The resolver types `!x` as `bool` whatever `x` is
    std::string_view{"Unary expression resolution"},
    // Two concatenated strings keep the left one's array type
    std::string_view{"Const eval string concatenation and comparison folding"},
    // A top-level template first reached from another instance's body is named as that
    // instance's local generic
    std::string_view{
        "a template first reached from another template's body stays a top-level function"},
};

// Tests where the old emitter builds a local `const` aggregate, or a compile-time capture or
// loop element, at runtime to read from it
constexpr std::array FOLDED_LOCAL_AGGREGATE_TESTS{
    std::string_view{
        "a match-captured payload's field is readable inside a nested `unroll for` body"},
    std::string_view{"a return type may instantiate a ctor that rejects an unbound `comptime T`"},
    std::string_view{"`++` concatenates two comptime integer arrays"},
    std::string_view{"`++` concatenates two function-local comptime array identifiers"},
    // The old emitter runs a call whose string arguments are known at compile time, one instance
    // per argument type
    std::string_view{
        "`eql`-shaped comptime fn: early length mismatch and content mismatch both fold correctly"},
    std::string_view{
        "a slice `auto` pattern takes an array or a reference to one, as a `[]T` does"},
    std::string_view{"@[discardable] on an interface member covers its implementations"},
};

// Tests where the old emitter's GIR only has another shape: a known value left to runtime, a
// value copied through one more slot, or a constant spelled with other digits
constexpr std::array OLD_SHAPE_TESTS{
    // `@as(usize, -1)` is the constant `-1`, which UIR spells as the unsigned all-ones value
    std::string_view{
        "Module-scope @ptrFromInt with a negative literal folds to an all-ones pointer"},
    // `@intCast` range checks and negation past 128 bits stay at runtime
    std::string_view{
        "multiplying and dividing integers past the inline width goes through compiler_rt"},
    // `@intFromFloat` of a call returning a known untyped float stays at runtime
    std::string_view{"A `@TypeOf(x)` parameter follows a comptime_int `x`"},
    // A block's range value is copied into one more slot
    std::string_view{"a range nested inside a `for` iterable or subscript is a range value"},
    // A string literal stored into a `[]u8` element is copied into a mutable stack array first
    std::string_view{"Array copy of pointer and slice elements still aliases their pointees"},
    // A `defer` body's captures follow the captures of the code after it, since AstGen lowers
    // the body at each exit
    std::string_view{"A generic closure body runs its `defer`s on each return"},
};

// Tests where the old emitter folds a whole `let` whose initializer is known, which UIR builds
// at runtime and reads, as the `let` rule decides
constexpr std::array WHOLE_LET_FOLD_TESTS{
    std::string_view{"Same-named `const` aggregates in two modules keep their own storage"},
    std::string_view{"@typeInfo and @Union carry a union's tag enum"},
    std::string_view{"Range copies and element writes through a range fold at compile time"},
    std::string_view{"a folded generic member call sees its own instance's `@This()`"},
    std::string_view{"A union tagged by an enum reads as that enum"},
    std::string_view{"scoped expressions work as member-access objects and initializer types"},
    std::string_view{"Vector unary, bitwise, shift, wrapping, and saturating operators"},
};

[[nodiscard]] auto folds_local_aggregate() -> bool {
    const auto name{Catch::getResultCapture().getCurrentTestName()};
    return std::ranges::contains(FOLDED_LOCAL_AGGREGATE_TESTS, std::string_view{name}) ||
           std::ranges::contains(OLD_SHAPE_TESTS, std::string_view{name}) ||
           std::ranges::contains(WHOLE_LET_FOLD_TESTS, std::string_view{name});
}

[[nodiscard]] auto shows_legacy_bug() -> bool {
    return std::ranges::contains(LEGACY_BUG_TESTS,
                                 std::string_view{Catch::getResultCapture().getCurrentTestName()});
}

// Every error is one the plan decided UIR reports where the old pipeline didn't
[[nodiscard]] auto only_decided_rejections(const sema::diagnostics& diags) -> bool {
    return std::ranges::all_of(diags, [](const sema::diagnostic& d) {
        const auto message{d.get_message().value_or(std::string{})};
        return (d.get_error() == sema::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE &&
                message == "`comptime let mut` assignment depends on a runtime condition") ||
               (d.get_error() == sema::error::ASSIGNMENT_TO_CONST &&
                message == "Cannot take a mutable address of an element of a non-mutable array "
                           "or slice") ||
               (d.get_error() == sema::error::ASSIGNMENT_TO_CONST &&
                message == "Cannot capture an immutable value by mutable reference") ||
               // The old resolver lets a type, like an enum, be matched against variant patterns
               (d.get_error() == sema::error::ILLEGAL_MATCH_PATTERN &&
                (message == "A 'match' on a type requires a catch-all '_' arm" ||
                 message == "A 'match' on a type expects every arm pattern to be a type"));
    });
}

// Some old checks report without marking their module errored
[[nodiscard]] auto has_errors(const sema::diagnostics& diags) -> bool {
    return std::ranges::any_of(diags, [](const sema::diagnostic& d) {
        return d.snapshot().level == diagnostic_level::ERROR;
    });
}

auto configure(sema::analyzer& analyzer, const pipeline_settings& settings) -> void {
    analyzer.set_testing(settings.testing);
    analyzer.set_optimize_mode(settings.build_mode);
    analyzer.set_entry_point(settings.entry && !settings.testing);
    analyzer.set_deprecation_policy(settings.deprecated_policy);
}

[[nodiscard]] auto make_manager(mod::source_loader& loader, library_list libraries)
    -> stdx::box<mod::module_manager> {
    auto modules{stdx::make_box<mod::module_manager>(loader)};
    for (const auto& [name, path] : libraries) { DISCARD(modules->add_library_module(name, path)); }
    return modules;
}

// The old pipeline's GIR dump, or none when it rejects the program
[[nodiscard]] auto legacy_gir(mod::source_loader&          loader,
                              const std::filesystem::path& root,
                              library_list                 libraries,
                              const pipeline_settings&     settings) -> stdx::option<std::string> {
    auto               modules{make_manager(loader, libraries)};
    std::ostringstream quiet;
    sema::analyzer     analyzer{
        *modules, quiet, false, settings.target_opts, false, settings.runtime_safety};
    configure(analyzer, settings);
    auto       gir_mod{analyzer.analyze(root, settings.entry && settings.testing)};
    const auto module{modules->try_get_file_module(root)};
    if (!gir_mod || !module || (*module)->is_errored() || (*module)->is_poisoned() ||
        modules->any_errored() || has_errors(analyzer.get_ctx().diags)) {
        return stdx::none;
    }
    return dump_with_flags(*gir_mod);
}

} // namespace

auto uir_diff_status_name(uir_diff_status status) noexcept -> std::string_view {
    switch (status) {
    case uir_diff_status::MATCH:            return "match";
    case uir_diff_status::UNSUPPORTED:      return "unsupported";
    case uir_diff_status::REJECTED:         return "rejected";
    case uir_diff_status::FOLD_PLACEMENT:   return "fold-placement";
    case uir_diff_status::SEMANTICS_CHANGE: return "semantics-change";
    case uir_diff_status::ON_USE_IMPL:      return "on-use-impl";
    case uir_diff_status::LEGACY_BUG:       return "legacy-bug";
    case uir_diff_status::GIR_DIFF:         return "gir-diff";
    case uir_diff_status::ASTGEN_FAILED:    return "astgen-failed";
    case uir_diff_status::LEGACY_REJECTED:  return "legacy-rejected";
    case uir_diff_status::EVAL_ORDER:       return "eval-order";
    }
    return "unknown";
}

auto diff_pipelines(mod::source_loader&          loader,
                    const std::filesystem::path& root,
                    library_list                 libraries,
                    const pipeline_settings&     settings) -> uir_diff {
    const auto legacy{legacy_gir(loader, root, libraries, settings)};
    if (!legacy) { return {.status = uir_diff_status::LEGACY_REJECTED}; }

    auto                    modules{make_manager(loader, libraries)};
    const sema::cfg_context cfg{.target_opts = settings.target_opts,
                                .facts =
                                    codegen::target_facts::resolve(settings.target_opts.triple_str),
                                .build_mode     = settings.build_mode,
                                .runtime_safety = settings.runtime_safety,
                                .testing        = settings.testing,
                                .modules        = *modules};
    const auto              root_mod{modules->try_get_file_module(root)};
    if (!root_mod) { return {.status = uir_diff_status::LEGACY_REJECTED}; }
    auto lowered{uir::lower_reachable(**root_mod, {.cfg = cfg, .modules = *modules})};
    if (settings.entry) { REQUIRE(uir::lower_start({.cfg = cfg, .modules = *modules}, lowered)); }
    for (const auto& item : lowered) {
        if (!item.result.diagnostics.empty()) {
            return {.status = uir_diff_status::ASTGEN_FAILED,
                    .detail = fmt::format("{}", item.result.diagnostics)};
        }
    }

    std::ostringstream quiet;
    sema::analyzer     analyzer{
        *modules, quiet, false, settings.target_opts, false, settings.runtime_safety};
    configure(analyzer, settings);
    auto out{uir::sema::analyze(analyzer.get_ctx(), {.root = root_mod->get(), .modules = lowered})};
    if (out.stopped_at) {
        // Where it stopped follows a tab, so a report still groups by what it stopped at
        const auto& stop{*out.stopped_at};
        const auto  where{stop.where.transform([&](const source_location& at) {
            const auto one{at.one_based()};
            return fmt::format("\t{}:{}:{}", stop.path.generic_string(), one.line, one.column);
        })};
        return {.status = uir_diff_status::UNSUPPORTED,
                .detail = stop.what + where.value_or(std::string{})};
    }
    if (!out.succeeded()) {
        const auto status{only_decided_rejections(out.diagnostics)
                              ? uir_diff_status::SEMANTICS_CHANGE
                          : shows_legacy_bug() ? uir_diff_status::LEGACY_BUG
                                               : uir_diff_status::REJECTED};
        return {.status = status, .detail = fmt::format("{}", out.diagnostics)};
    }

    // Normalizing is the costly part, so it waits until both pipelines produced GIR
    const auto legacy_text{normalize_gir(*legacy)};
    const auto uir_text{[&] {
        auto text{normalize_gir(dump_with_flags(*out.gir))};
        auto renamed{with_legacy_ctor_names(legacy_text, text)};
        // Renaming may reorder functions, which normalizing sorts again
        return renamed == text ? text : normalize_gir(renamed);
    }()};
    if (uir_text == legacy_text) { return {.status = uir_diff_status::MATCH}; }

    // `GHOTI_UIR_DIFF_DUMP=<dir>` keeps both normalized dumps of every program that differs
    if (const auto dir{get_env("GHOTI_UIR_DIFF_DUMP")}) {
        const auto stem{fmt::format("{}", stdx::hash<std::string>{}(root.generic_string()))};
        const auto base{std::filesystem::path{*dir} / root.filename()};
        std::ofstream{base.string() + "." + stem + ".old"} << legacy_text;
        std::ofstream{base.string() + "." + stem + ".uir"} << uir_text;
    }

    if (shows_legacy_bug()) { return {.status = uir_diff_status::LEGACY_BUG}; }
    if (folds_local_aggregate()) { return {.status = uir_diff_status::FOLD_PLACEMENT}; }

    // The `let` rule only reads one module, since across an import it would excuse any diff
    if (const auto placed{only_fold_placement(
            uir_text, legacy_text, lowered.size() == 1 && has_folded_let(lowered))}) {
        return {.status = *placed};
    }
    if (without_all_unused(legacy_text, uir_text) == uir_text) {
        return {.status = uir_diff_status::ON_USE_IMPL};
    }
    if (has_folded_let(lowered) || has_folded_call(uir_text, legacy_text)) {
        return {.status = uir_diff_status::FOLD_PLACEMENT};
    }
    return {.status = uir_diff_status::GIR_DIFF,
            .detail = fmt::format("--- old\n{}--- uir\n{}", legacy_text, uir_text)};
}

auto run_uir_sema(std::string_view source, const pipeline_settings& settings)
    -> stdx::box<uir_sema_run> {
    constexpr std::string_view ROOT{"test.gh"};
    auto                       loader{stdx::make_box<mod::memory_loader>()};
    loader->add(ROOT, std::string{source});
    auto run{run_uir_sema_files(*loader, ROOT, settings)};
    run->loader = std::move(loader);
    return run;
}

auto run_uir_sema_files(mod::source_loader&          loader,
                        const std::filesystem::path& root_path,
                        const pipeline_settings&     settings,
                        library_list                 libraries) -> stdx::box<uir_sema_run> {
    auto run{stdx::make_box<uir_sema_run>()};
    run->modules  = make_manager(loader, libraries);
    run->analyzer = stdx::make_box<sema::analyzer>(*run->modules, std::cerr, false);
    const auto root{run->modules->try_get_file_module(root_path)};
    REQUIRE(root);
    REQUIRE_FALSE((*root)->is_errored());
    configure(*run->analyzer, settings);
    auto& ctx{run->analyzer->get_ctx()};
    ctx.target_opts    = settings.target_opts;
    ctx.runtime_safety = settings.runtime_safety;
    const sema::cfg_context cfg{.target_opts = ctx.target_opts,
                                .facts =
                                    codegen::target_facts::resolve(settings.target_opts.triple_str),
                                .build_mode     = ctx.build_mode,
                                .runtime_safety = ctx.runtime_safety,
                                .testing        = settings.testing,
                                .modules        = *run->modules};
    run->lowered = uir::lower_reachable(**root, {.cfg = cfg, .modules = *run->modules});
    if (settings.entry) {
        REQUIRE(uir::lower_start({.cfg = cfg, .modules = *run->modules}, run->lowered));
    }
    for (const auto& item : run->lowered) { REQUIRE(item.result.diagnostics.empty()); }
    run->out.emplace(
        uir::sema::analyze(run->analyzer->get_ctx(),
                           {.root = root->get(), .modules = run->lowered},
                           {.check_all = settings.check_all, .index = settings.index}));
    if (settings.index) {
        for (auto& item : run->lowered) { uir::index_names(item); }
    }
    return run;
}

auto uir_diagnostics(mod::source_loader&          loader,
                     const std::filesystem::path& root_path,
                     const pipeline_settings&     settings,
                     library_list                 libraries) -> program_diagnostics {
    auto       modules{make_manager(loader, libraries)};
    const auto root{modules->try_get_file_module(root_path)};
    REQUIRE(root);
    REQUIRE_FALSE((*root)->is_errored());

    sema::analyzer analyzer{*modules, std::cerr, false};
    configure(analyzer, settings);
    auto& ctx{analyzer.get_ctx()};
    ctx.target_opts    = settings.target_opts;
    ctx.runtime_safety = settings.runtime_safety;
    const sema::cfg_context cfg{.target_opts = ctx.target_opts,
                                .facts = codegen::target_facts::resolve(settings.target_opts.triple_str),
                                .build_mode     = ctx.build_mode,
                                .runtime_safety = ctx.runtime_safety,
                                .testing        = settings.testing,
                                .modules        = *modules};

    // AstGen's errors stop the build before Sema, as they do in the driver
    program_diagnostics out;
    auto lowered{uir::lower_reachable(**root, {.cfg = cfg, .modules = *modules})};
    for (auto& item : lowered) {
        for (auto& d : item.result.diagnostics) { out.errors.emplace_back(std::move(d)); }
    }
    if (!out.errors.empty()) { return out; }

    auto analyzed{uir::sema::analyze(ctx,
                                     {.root = root->get(), .modules = lowered},
                                     {.check_all = settings.check_all})};
    if (analyzed.stopped_at) { FAIL("UIR Sema stopped at " << analyzed.stopped_at->what); }
    out.errors   = std::move(analyzed.diagnostics);
    out.warnings = std::move(analyzed.warnings);
    return out;
}

auto uir_gir(std::string_view source) -> std::string {
    const auto  run{run_uir_sema(source)};
    const auto& out{*run->out};
    if (out.stopped_at) { FAIL("UIR Sema stopped at " << out.stopped_at->what); }
    if (!out.succeeded()) { FAIL("UIR Sema reported:\n" << fmt::format("{}", out.diagnostics)); }
    return normalize_gir(dump_with_flags(*out.gir));
}

auto report_uir_diff(const uir_diff& diff, std::string_view label) -> void {
    const auto path{get_env("GHOTI_TEST_UIR_DIFF_REPORT")};
    if (!path) { return; }
    std::ofstream out{std::string{*path}, std::ios::app | std::ios::binary};
    std::string   flat{diff.status == uir_diff_status::GIR_DIFF ? "" : diff.detail};
    std::ranges::replace(flat, '\n', ' ');
    fmt::println(out,
                 "{}\t{}\t{}",
                 uir_diff_status_name(diff.status),
                 label.empty() ? Catch::getResultCapture().getCurrentTestName()
                               : std::string{label},
                 flat);
}

} // namespace ghoti::tests::helpers
