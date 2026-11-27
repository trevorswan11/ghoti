#include "helpers/gir_normalize.hh"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "support/float128.hh"
#include "support/int128.hh"
#include "support/string_utils.hh"

namespace ghoti::tests::helpers {

namespace {

// Whether a dump line starts a record of its own, not a later line of a multi-line string
[[nodiscard]] auto starts_record(std::string_view line) -> bool {
    if (line.empty()) { return true; }
    for (const auto prefix : {"fn ", "const ", "type ", "var ", "test \"", "  seg "}) {
        if (line.starts_with(prefix)) { return true; }
    }

    return line.starts_with("    ") && line.size() > 4 && line[4] != ' ';
}

// The dump with each multi-line string literal on the line of the instruction holding it
[[nodiscard]] auto joined_string_lines(std::string_view dump) -> std::string {
    std::string out;
    bool        first{true};
    for (usize at{0}; at < dump.size();) {
        const auto end{std::min(dump.find('\n', at), dump.size())};
        const auto line{dump.substr(at, end - at)};
        at = end + 1;
        if (!first && !starts_record(line)) {
            out += "\\n";
        } else if (!first) {
            out += '\n';
        }

        out += line;
        first = false;
    }

    if (dump.ends_with('\n')) { out += '\n'; }
    return out;
}

// A store's value and destination, split at the last comma since a string value may hold one
[[nodiscard]] auto store_args(std::string_view body) -> std::vector<std::string_view> {
    const auto operands{stdx::string::substr(body, 6)};
    const auto at{operands.rfind(", ")};
    if (at == std::string_view::npos) { return {operands}; }
    return {operands.substr(0, at), operands.substr(at + 2)};
}

struct segment_dump {
    u32                      id;
    std::vector<std::string> instructions;
};

struct function_dump {
    std::string               header;
    std::vector<segment_dump> segments;
};

struct module_dump {
    std::vector<std::string>   items;
    std::vector<function_dump> functions;
};

[[nodiscard]] auto parse_u32(std::string_view text) -> stdx::option<u32> {
    u32 value{0};
    const auto [ptr, ec]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (ec != std::errc{} || ptr != text.data() + text.size()) { return stdx::none; }
    return value;
}

[[nodiscard]] auto parse(std::string_view dump) -> module_dump {
    module_dump out;
    for (const auto line : string_utils::split(dump, "\n")) {
        if (line.empty()) { continue; }
        if (line.starts_with("fn ") || line.starts_with("test \"")) {
            out.functions.emplace_back<function_dump>(
                {.header = std::string{line}, .segments = {}});
        } else if (line.starts_with("  seg ") && line.ends_with(':') && !out.functions.empty()) {
            const auto id{parse_u32(stdx::string::substr(line, 6, line.size() - 7)).value_or(0)};
            out.functions.back().segments.emplace_back<segment_dump>(
                {.id = id, .instructions = {}});
        } else if (line.starts_with("    ") && !out.functions.empty() &&
                   !out.functions.back().segments.empty()) {
            out.functions.back().segments.back().instructions.emplace_back(
                stdx::string::substr(line, 4));
        } else {
            out.items.emplace_back(line);
        }
    }

    return out;
}

[[nodiscard]] constexpr auto is_digit(char c) noexcept -> bool { return c >= '0' && c <= '9'; }

[[nodiscard]] constexpr auto is_mangle_char(char c) noexcept -> bool {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

// The length of a nominal type's discriminator in a monomorph's symbol starting at `at`
[[nodiscard]] auto discriminator_length(std::string_view text, usize at) -> usize {
    if (at >= text.size()) { return 0; }
    usize end{at};
    // `$Ctor__args_<id>` ends at its instance id, before any parameter mangled after it
    if (text[at] == '$') {
        while (end < text.size() && is_mangle_char(text[end])) { ++end; }
        usize last_id{end};
        for (usize i{at + 1}; i < end; ++i) {
            if (text[i] != '_' || i + 1 >= end || !is_digit(text[i + 1])) { continue; }
            usize digits{i + 1};
            while (digits < end && is_digit(text[digits])) { ++digits; }
            if (digits == end || text[digits] == '_') { last_id = digits; }
        }

        return last_id - at;
    }

    while (end < text.size() && is_digit(text[end])) { ++end; }
    return end - at;
}

// The next `.slice_lit.N` or `.ref_lit.N` at or after `from`, as its start and length
[[nodiscard]] auto literal_global_at(std::string_view text, usize from)
    -> stdx::option<std::pair<usize, usize>> {
    stdx::option<std::pair<usize, usize>> best;
    for (const std::string_view prefix : {".slice_lit.", ".ref_lit."}) {
        for (auto at{text.find(prefix, from)}; at != std::string_view::npos;
             at = text.find(prefix, at + 1)) {
            usize end{at + prefix.size()};
            while (end < text.size() && is_digit(text[end])) { ++end; }
            if (end == at + prefix.size()) { continue; }
            if (!best || at < best->first) { best.emplace(at, end - at); }
            break;
        }
    }

    return best;
}

[[nodiscard]] constexpr auto is_hex(char c) noexcept -> bool {
    return is_digit(c) || (c >= 'a' && c <= 'f');
}

// The next local function's symbol at or after `from`, `closureN`, `localfn.N` or
// `anonymous_fnN` with any instance suffix, as its start and length
[[nodiscard]] auto local_function_at(std::string_view text, usize from)
    -> stdx::option<std::pair<usize, usize>> {
    stdx::option<std::pair<usize, usize>> best;
    for (const std::string_view prefix : {"closure", "localfn.", "anonymous_fn"}) {
        for (auto at{text.find(prefix, from)}; at != std::string_view::npos;
             at = text.find(prefix, at + 1)) {
            // A monomorph's symbol spells a closure argument between underscores
            if (at > 0 && is_mangle_char(text[at - 1]) && text[at - 1] != '_') { continue; }
            usize end{at + prefix.size()};
            while (end < text.size() && is_digit(text[end])) { ++end; }
            if (end == at + prefix.size()) { continue; }
            // The instance suffix, which a monomorph's symbol spells with an `x`
            const bool dotted{end + 1 < text.size() && text[end] == '.' && is_hex(text[end + 1])};
            const bool mangled{end + 17 <= text.size() && text[end] == 'x' &&
                               std::ranges::all_of(text.substr(end + 1, 16), is_hex)};
            if (dotted || mangled) {
                ++end;
                while (end < text.size() && is_hex(text[end])) { ++end; }
            }

            if (end < text.size() && is_mangle_char(text[end]) && text[end] != '_') { continue; }
            if (!best || at < best->first) { best.emplace(at, end - at); }
            break;
        }
    }

    return best;
}

// The text with every local function's number erased, which orders functions alike
[[nodiscard]] auto without_local_numbers(std::string_view text) -> std::string {
    std::string out;
    usize       at{0};
    while (const auto found{local_function_at(text, at)}) {
        const auto [start, len]{*found};
        out += text.substr(at, start - at);
        out += text.substr(start, 7) == "closure" ? "closure#" : "localfn.#";
        at = start + len;
    }

    out += text.substr(at);
    return out;
}

// A dump with every nominal type in a monomorph's symbol spelled `struct#`, every
// parameterized impl's method prefix spelled `pimpl#`, and every vtable `__vtable.#`
[[nodiscard]] auto erase_nominal_discriminators(std::string_view dump) -> std::string {
    static constexpr std::array       KINDS{"_struct", "_union", "_enum", "_gfn"};
    static constexpr std::string_view IMPL_PREFIX{"pimpl"};
    static constexpr std::string_view VTABLE_PREFIX{"__vtable."};

    std::string out;
    out.reserve(dump.size());
    for (usize i{0}; i < dump.size();) {
        // A vtable is numbered by each pipeline's own count
        if (stdx::string::substr(dump, i).starts_with(VTABLE_PREFIX)) {
            usize end{i + VTABLE_PREFIX.size()};
            while (end < dump.size() && is_digit(dump[end])) { ++end; }
            if (end > i + VTABLE_PREFIX.size()) {
                out += VTABLE_PREFIX;
                out += '#';
                i = end;
                continue;
            }
        }

        // A parameterized impl's methods carry the number of the impl they come from
        if (stdx::string::substr(dump, i).starts_with(IMPL_PREFIX)) {
            usize end{i + IMPL_PREFIX.size()};
            while (end < dump.size() && is_digit(dump[end])) { ++end; }
            if (end > i + IMPL_PREFIX.size() && end < dump.size() && dump[end] == '#') {
                out += IMPL_PREFIX;
                i = end;
                continue;
            }
        }

        // A qualified symbol names its scope by each pipeline's own number
        const bool word_start{i == 0 || !is_mangle_char(dump[i - 1])};
        if (word_start && dump[i] == 'm') {
            usize end{i + 1};
            while (end < dump.size() && is_digit(dump[end])) { ++end; }
            if (end > i + 1 && end < dump.size() && dump[end] == '.') {
                out += "m#";
                i = end;
                continue;
            }
        }

        // A generic instance's local generics are numbered by each pipeline's own count, which
        // the old pipeline writes without leading zeros
        if (stdx::string::substr(dump, i).starts_with("_i") && i > 0 &&
            is_mangle_char(dump[i - 1])) {
            usize end{i + 2};
            while (end < dump.size() && end < i + 18 && is_hex(dump[end])) { ++end; }
            const bool ends{end == dump.size() || !is_mangle_char(dump[end]) || dump[end] == '_'};
            if (end >= i + 14 && ends) {
                out += "_i#";
                i = end;
                continue;
            }
        }

        // A second same-named generic's instances are numbered by when each was first instantiated
        if (stdx::string::substr(dump, i).starts_with("_v") && i > 0 &&
            is_mangle_char(dump[i - 1])) {
            usize end{i + 2};
            while (end < dump.size() && is_digit(dump[end])) { ++end; }
            if (end > i + 2 && stdx::string::substr(dump, end).starts_with("__")) {
                i = end;
                continue;
            }
        }

        bool erased{false};
        for (const std::string_view kind : KINDS) {
            if (!stdx::string::substr(dump, i).starts_with(kind)) { continue; }
            const auto length{discriminator_length(dump, i + kind.size())};
            if (length == 0) { continue; }
            out += kind;
            out += '#';
            i += kind.size() + length;
            erased = true;
            break;
        }

        if (!erased) { out += dump[i++]; }
    }

    return out;
}

// A value is never volatile, only the memory it is read from, so a value instruction's type is
// spelled without the qualifier the old pipeline sometimes carries over from it
[[nodiscard]] auto without_value_volatility(std::string inst) -> std::string {
    const auto assign{inst.find(" = ")};
    const auto op_start{inst.starts_with('%') && assign != std::string::npos ? assign + 3 : 0};
    const auto op_end{inst.find(' ', op_start)};
    if (op_end == std::string::npos) { return inst; }
    const std::string_view op{stdx::string::substr(inst, op_start, op_end - op_start)};
    for (const std::string_view memory : {"alloca", "get_element_ptr", "address_of", "load"}) {
        if (op == memory) { return inst; }
    }

    for (const std::string_view qualifier : {"mut volatile ", "volatile "}) {
        if (stdx::string::substr(inst, op_end + 1).starts_with(qualifier)) {
            inst.erase(op_end + 1, qualifier.size());
            break;
        }
    }

    return inst;
}

// A call through a function's own symbol as a constant is the direct call it amounts to
[[nodiscard]] auto as_direct_call(std::string inst) -> std::string {
    const auto at{inst.find("call \"")};
    if (at == std::string::npos) { return inst; }
    const auto name_at{at + 6};
    const auto close{inst.find("\"(", name_at)};
    if (close == std::string::npos) { return inst; }
    const auto name{inst.substr(name_at, close - name_at)};
    if (name.empty() || name.find_first_of(" \"") != std::string::npos) { return inst; }
    return inst.substr(0, at) + "call @" + name + inst.substr(close + 1);
}

// A field or element address with no mutability in its type, which only Sema reads. A callable
// made from a function's code doesn't write through it either
[[nodiscard]] auto without_address_mutability(std::string_view inst) -> std::string {
    constexpr std::string_view MUTABLE_CALLABLE{"make_callable ^mut fn"};
    if (const auto callable{inst.find(MUTABLE_CALLABLE)}; callable != std::string_view::npos) {
        return fmt::format("{}make_callable ^fn{}",
                           stdx::string::substr(inst, 0, callable),
                           stdx::string::substr(inst, callable + MUTABLE_CALLABLE.size()));
    }

    const auto at{inst.find("get_element_ptr ")};
    if (at == std::string_view::npos) { return std::string{inst}; }
    const auto type_start{at + std::string_view{"get_element_ptr "}.size()};
    // The type ends where the base operand starts: a temporary or a parameter
    const auto temp{inst.find(" %", type_start)};
    const auto param{inst.find(" param.", type_start)};
    const auto type_end{std::min(temp, param)};
    if (type_end == std::string_view::npos) { return std::string{inst}; }

    std::string type{stdx::string::substr(inst, type_start, type_end - type_start)};
    for (const auto qualifier : {std::string_view{"mut? "}, std::string_view{"mut "}}) {
        for (auto found{type.find(qualifier)}; found != std::string::npos;
             found = type.find(qualifier)) {
            type.erase(found, qualifier.size());
        }
    }

    return fmt::format("{}{}{}",
                       stdx::string::substr(inst, 0, type_start),
                       type,
                       stdx::string::substr(inst, type_end));
}

// Calls `f` on each `%N` token in `text`, rewriting it to whatever `f` returns
template <typename F>
[[nodiscard]] auto rewrite_temps(std::string_view text, F&& f) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (usize i{0}; i < text.size();) {
        if (text[i] == '%' && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9') {
            usize end{i + 1};
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; }
            out += f(stdx::string::substr(text, i, end - i));
            i = end;
        } else {
            out += text[i++];
        }
    }

    return out;
}

// The temporaries `text` mentions, in the order it mentions them
[[nodiscard]] auto mentioned_temps(std::string_view text) -> std::vector<std::string> {
    std::vector<std::string> out;
    DISCARD(rewrite_temps(text, [&](std::string_view t) -> std::string {
        out.emplace_back(t);
        return std::string{t};
    }));
    return out;
}

// The temporary an instruction defines, and the rest of its text
[[nodiscard]] auto split_definition(std::string_view inst)
    -> std::pair<std::string_view, std::string_view> {
    if (!inst.starts_with('%')) { return {{}, inst}; }
    const auto eq{inst.find(" = ")};
    if (eq == std::string_view::npos) { return {{}, inst}; }
    return {stdx::string::substr(inst, 0, eq), stdx::string::substr(inst, eq + 3)};
}

struct int_shape {
    u16  bits;
    bool is_signed;
};

[[nodiscard]] auto int_shape_of(std::string_view type) -> stdx::option<int_shape> {
    if (type == "usize") { return int_shape{64, false}; }
    if (type == "isize") { return int_shape{64, true}; }
    if (type.size() < 2 || (type[0] != 'i' && type[0] != 'u')) { return stdx::none; }
    const auto bits{parse_u32(stdx::string::substr(type, 1))};
    if (!bits || *bits == 0 || *bits > 64) { return stdx::none; }
    return int_shape{static_cast<u16>(*bits), type[0] == 'i'};
}

[[nodiscard]] auto parse_int(std::string_view text) -> stdx::option<i128> {
    const bool negative{text.starts_with('-')};
    if (negative) { text.remove_prefix(1); }
    if (text.empty() || text.size() > 30) { return stdx::none; }
    i128 value{0};
    for (const char c : text) {
        if (c < '0' || c > '9') { return stdx::none; }
        value = value * 10 + (c - '0');
    }

    return negative ? -value : value;
}

[[nodiscard]] auto format_int(i128 value) -> std::string {
    if (value == 0) { return "0"; }
    const bool  negative{value < 0};
    u128        magnitude{negative ? static_cast<u128>(-value) : static_cast<u128>(value)};
    std::string digits;
    while (magnitude != 0) {
        digits += static_cast<char>('0' + static_cast<int>(magnitude % 10));
        magnitude /= 10;
    }

    if (negative) { digits += '-'; }
    std::ranges::reverse(digits);
    return digits;
}

[[nodiscard]] auto min_of(int_shape s) -> i128 {
    return s.is_signed ? -(i128{1} << (s.bits - 1)) : i128{0};
}
[[nodiscard]] auto max_of(int_shape s) -> i128 {
    return s.is_signed ? (i128{1} << (s.bits - 1)) - 1 : (i128{1} << s.bits) - 1;
}

// `value`'s low `bits`, read back in the shape
[[nodiscard]] auto wrap(i128 value, int_shape s) -> i128 {
    const u128 mask{(u128{1} << s.bits) - 1};
    const u128 low{static_cast<u128>(value) & mask};
    if (s.is_signed && (low >> (s.bits - 1)) != 0) {
        return static_cast<i128>(low) - static_cast<i128>(u128{1} << s.bits);
    }

    return static_cast<i128>(low);
}

struct fold_input {
    std::string_view              kind;
    bool                          saturating;
    bool                          checked;
    std::string_view              type;
    std::vector<std::string_view> operands;
};

[[nodiscard]] auto parse_fold_input(std::string_view body) -> stdx::option<fold_input> {
    const auto space{body.find(' ')};
    if (space == std::string_view::npos) { return stdx::none; }
    auto       name{stdx::string::substr(body, 0, space)};
    auto       rest{stdx::string::substr(body, space + 1)};
    fold_input in{};
    in.kind       = stdx::string::substr(name, 0, name.find('.'));
    in.saturating = name.find(".sat") != std::string_view::npos;
    in.checked    = name.find(".checked") != std::string_view::npos;
    const auto type_end{rest.find(' ')};
    if (type_end == std::string_view::npos) { return stdx::none; }
    in.type     = stdx::string::substr(rest, 0, type_end);
    in.operands = string_utils::split(stdx::string::substr(rest, type_end + 1), ", ");
    return in;
}

[[nodiscard]] auto is_bool_constant(std::string_view v) -> bool {
    return v == "true" || v == "false";
}

// A decimal integer or float constant as the dumper spells it
[[nodiscard]] auto is_number_text(std::string_view v) -> bool {
    if (v.starts_with('-')) { v.remove_prefix(1); }
    if (v.empty() || v.front() < '0' || v.front() > '9') { return false; }
    return std::ranges::all_of(v, [](char c) {
        return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == '+' || c == '-';
    });
}

// The constant an all-constant integer or bool instruction computes, none when it can't be
// folded exactly
[[nodiscard]] auto fold(const fold_input& in) -> stdx::option<std::string> {
    const auto& ops{in.operands};
    if (in.kind == "constant" && ops.size() == 1 &&
        (is_bool_constant(ops[0]) || parse_int(ops[0]))) {
        return std::string{ops[0]};
    }

    if (in.type == "bool") {
        if (in.kind == "not" && ops.size() == 1 && is_bool_constant(ops[0])) {
            return std::string{ops[0] == "true" ? "false" : "true"};
        }

        if (ops.size() != 2) { return stdx::none; }
        if (is_bool_constant(ops[0]) && is_bool_constant(ops[1])) {
            if (in.kind == "eq") { return std::string{ops[0] == ops[1] ? "true" : "false"}; }
            if (in.kind == "ne") { return std::string{ops[0] != ops[1] ? "true" : "false"}; }
            if (in.kind == "and") {
                return std::string{ops[0] == "true" && ops[1] == "true" ? "true" : "false"};
            }

            if (in.kind == "or") {
                return std::string{ops[0] == "true" || ops[1] == "true" ? "true" : "false"};
            }

            return stdx::none;
        }

        const auto l{parse_int(ops[0])};
        const auto r{parse_int(ops[1])};
        if (!l || !r) { return stdx::none; }
        const auto truth{[&] -> stdx::option<bool> {
            if (in.kind == "eq") { return *l == *r; }
            if (in.kind == "ne") { return *l != *r; }
            if (in.kind == "lt") { return *l < *r; }
            if (in.kind == "le") { return *l <= *r; }
            if (in.kind == "gt") { return *l > *r; }
            if (in.kind == "ge") { return *l >= *r; }
            return stdx::none;
        }()};
        if (!truth) { return stdx::none; }
        return std::string{*truth ? "true" : "false"};
    }

    // An untyped operation the old emitter left at runtime computes exactly
    if (in.type == "comptime_int" && ops.size() == 2) {
        const auto     l{parse_int(ops[0])};
        const auto     r{parse_int(ops[1])};
        constexpr i128 LIMIT{i128{1} << 62};
        if (!l || !r || *l > LIMIT || *l < -LIMIT || *r > LIMIT || *r < -LIMIT) {
            return stdx::none;
        }

        if (in.kind == "add") { return format_int(*l + *r); }
        if (in.kind == "sub") { return format_int(*l - *r); }
        if (in.kind == "mul") { return format_int(*l * *r); }
        return stdx::none;
    }

    // Widening a known number to a float is exact, so it reads as the number
    const bool float_type{in.type == "f16" || in.type == "f32" || in.type == "f64" ||
                          in.type == "f80" || in.type == "f128"};
    if (float_type && in.kind == "widen_cast" && ops.size() == 1 && is_number_text(ops[0])) {
        return std::string{ops[0]};
    }

    // Arithmetic on known floats rounds once into the instruction's format
    if (float_type && ops.size() == 2 && is_number_text(ops[0]) && is_number_text(ops[1]) &&
        (in.kind == "add" || in.kind == "sub" || in.kind == "mul")) {
        const auto format{in.type == "f16"   ? float_format::HALF
                          : in.type == "f32" ? float_format::SINGLE
                          : in.type == "f64" ? float_format::DOUBLE
                          : in.type == "f80" ? float_format::X87
                                             : float_format::QUAD};
        const auto l{f128::parse(ops[0])};
        const auto r{f128::parse(ops[1])};
        if (l.status != float_parse_status::OK || r.status != float_parse_status::OK) {
            return stdx::none;
        }

        const auto value{in.kind == "add"   ? add(l.value, r.value, format)
                         : in.kind == "sub" ? subtract(l.value, r.value, format)
                                            : multiply(l.value, r.value, format)};
        return value.to_string();
    }

    const auto shape{int_shape_of(in.type)};
    if (!shape) { return stdx::none; }
    if (ops.size() == 1) {
        const auto v{parse_int(ops[0])};
        if (!v) { return stdx::none; }
        if (in.kind == "widen_cast") { return format_int(*v); }

        // A cast of a known integer the old emitter left at runtime is that integer, which an
        // `@intCast` checked fits at compile time
        if (in.kind == "int_cast" && *v >= min_of(*shape) && *v <= max_of(*shape)) {
            return format_int(*v);
        }

        if (in.kind == "trunc_cast") { return format_int(wrap(*v, *shape)); }
        if (in.kind == "bitnot") { return format_int(wrap(~*v, *shape)); }
        if (in.kind == "neg") {
            if (in.checked && -*v > max_of(*shape)) { return stdx::none; }
            return format_int(wrap(-*v, *shape));
        }

        return stdx::none;
    }

    if (ops.size() != 2) { return stdx::none; }
    const auto l{parse_int(ops[0])};
    const auto r{parse_int(ops[1])};
    if (!l || !r) { return stdx::none; }

    const auto exact{[&] -> stdx::option<i128> {
        if (in.kind == "add") { return *l + *r; }
        if (in.kind == "sub") { return *l - *r; }
        if (in.kind == "mul") { return *l * *r; }
        if (in.kind == "div" && *r > 0 && *l >= 0) { return *l / *r; }
        if (in.kind == "mod" && *r > 0 && *l >= 0) { return *l % *r; }
        if (in.kind == "and") { return wrap(*l, *shape) & wrap(*r, *shape); }
        if (in.kind == "or") { return wrap(*l, *shape) | wrap(*r, *shape); }
        if (in.kind == "xor") { return wrap(*l, *shape) ^ wrap(*r, *shape); }
        if ((in.kind == "shl" || in.kind == "shr") && *r >= 0 && *r < shape->bits) {
            if (in.kind == "shr") { return *l >> static_cast<int>(*r); }
            return *l * (i128{1} << static_cast<int>(*r));
        }

        return stdx::none;
    }()};
    if (!exact) { return stdx::none; }
    const bool fits{*exact >= min_of(*shape) && *exact <= max_of(*shape)};
    if (in.saturating) { return format_int(std::clamp(*exact, min_of(*shape), max_of(*shape))); }
    if (!fits && in.checked) { return stdx::none; }
    return format_int(wrap(*exact, *shape));
}

[[nodiscard]] auto is_constant_token(std::string_view v) -> bool {
    return !v.empty() && v.find('%') == std::string_view::npos && !v.starts_with("param.") &&
           !v.starts_with('@') && v != "undefined";
}

using known_map = ankerl::unordered_dense::map<std::string, std::string>;

[[nodiscard]] auto successors(std::string_view terminator) -> std::vector<u32>;

// Segments no jump from the entry reaches, which a folded branch leaves behind
auto drop_unreachable(function_dump& fn) -> void {
    if (fn.segments.empty()) { return; }
    ankerl::unordered_dense::map<u32, usize> by_id;
    for (usize i{0}; i < fn.segments.size(); ++i) { by_id.insert_or_assign(fn.segments[i].id, i); }
    ankerl::unordered_dense::set<u32> reached;
    for (std::vector<u32> stack{fn.segments.front().id}; !stack.empty();) {
        const auto id{stack.back()};
        stack.pop_back();
        if (!by_id.contains(id) || !reached.insert(id).second) { continue; }
        const auto& insts{fn.segments[by_id[id]].instructions};
        if (!insts.empty()) {
            for (const auto s : successors(insts.back())) { stack.emplace_back(s); }
        }
    }

    std::erase_if(fn.segments, [&](const segment_dump& seg) { return !reached.contains(seg.id); });
}

// A store another store to the same slot replaces, in the same segment, before anything reads it
auto drop_overwritten_stores(function_dump& fn) -> void {
    for (auto& seg : fn.segments) {
        ankerl::unordered_dense::map<std::string, usize> pending;
        std::vector<bool>                                dead(seg.instructions.size(), false);
        for (usize i{0}; i < seg.instructions.size(); ++i) {
            const std::string_view inst{seg.instructions[i]};
            if (inst.starts_with("store ")) {
                const auto args{string_utils::split(stdx::string::substr(inst, 6), ", ")};
                if (args.size() == 2 && args[1].starts_with('%')) {
                    const std::string slot{args[1]};
                    if (const auto found{pending.find(slot)}; found != pending.end()) {
                        dead[found->second] = true;
                    }

                    pending.erase(std::string{args[0]});
                    pending.insert_or_assign(slot, i);
                    continue;
                }
            }

            // Any other mention of a slot may read it
            DISCARD(rewrite_temps(inst, [&](std::string_view t) -> std::string {
                pending.erase(std::string{t});
                return std::string{t};
            }));
        }

        std::vector<std::string> kept;
        for (usize i{0}; i < seg.instructions.size(); ++i) {
            if (!dead[i]) { kept.emplace_back(std::move(seg.instructions[i])); }
        }

        seg.instructions = std::move(kept);
    }
}

// A widening cast of a value already of its type, which the old emitter makes for `@as` of a
// vector, is the value itself
auto drop_identity_casts(function_dump& fn) -> void {
    ankerl::unordered_dense::map<std::string, std::string> defined;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (!def.empty()) { defined.insert_or_assign(std::string{def}, std::string{body}); }
        }
    }

    constexpr std::string_view                             CAST{"widen_cast "};
    ankerl::unordered_dense::map<std::string, std::string> same;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (def.empty() || !body.starts_with(CAST)) { continue; }
            const auto rest{body.substr(CAST.size())};
            const auto space{rest.rfind(' ')};
            if (space == std::string_view::npos) { continue; }
            const auto        type{rest.substr(0, space)};
            const std::string operand{rest.substr(space + 1)};
            const auto        found{defined.find(operand)};
            if (found == defined.end()) { continue; }
            const std::string_view source{found->second};
            const auto             opcode_end{source.find(' ')};
            if (opcode_end != std::string_view::npos &&
                source.substr(opcode_end + 1).starts_with(fmt::format("{} ", type))) {
                same.insert_or_assign(std::string{def}, operand);
            }
        }
    }
    if (same.empty()) { return; }

    for (auto& seg : fn.segments) {
        std::erase_if(seg.instructions, [&](const std::string& inst) {
            return same.contains(std::string{split_definition(inst).first});
        });
        for (auto& inst : seg.instructions) {
            inst = rewrite_temps(inst, [&](std::string_view t) -> std::string {
                const auto found{same.find(std::string{t})};
                return found == same.end() ? std::string{t} : found->second;
            });
        }
    }
}

// An instruction that only computes its result
[[nodiscard]] auto is_pure(std::string_view body) -> bool {
    static constexpr std::array prefixes{
        "load ",         "get_element_ptr ",
        "global_addr ",  "address_of ",
        "not ",          "eq ",
        "ne ",           "lt ",
        "le ",           "gt ",
        "ge ",           "and ",
        "or ",           "xor ",
        "widen_cast ",   "bit_cast ",
        "int_cast ",     "trunc_cast ",
        "ptr_cast ",     "int_from_ptr ",
        "ptr_from_int ", "add ",
        "sub ",          "mul ",
        "shl ",          "shr ",
        "splat ",        "extract_lane ",
        "insert_lane ",  "repack ",
        "shuffle ",      "select ",
        "reduce ",
    };
    return std::ranges::any_of(prefixes, [&](std::string_view p) { return body.starts_with(p); });
}

// A load, address or computation nothing reads, and then a slot only ever stored to
auto drop_dead_slots(function_dump& fn) -> void {
    bool changed{true};
    while (changed) {
        changed = false;
        ankerl::unordered_dense::map<std::string, usize> uses;
        ankerl::unordered_dense::map<std::string, usize> stored_into;

        // Only a slot's own stores are dead with it, not stores through an address into memory
        ankerl::unordered_dense::set<std::string> slots;
        for (const auto& seg : fn.segments) {
            for (const auto& inst : seg.instructions) {
                const auto [def, body]{split_definition(inst)};
                if (!def.empty() && body.starts_with("alloca ")) { slots.emplace(def); }
                DISCARD(rewrite_temps(body, [&](std::string_view t) -> std::string {
                    ++uses[std::string{t}];
                    return std::string{t};
                }));
                if (body.starts_with("store ")) {
                    const auto args{store_args(body)};
                    if (args.size() == 2 && args[1].starts_with('%') && args[0] != args[1]) {
                        ++stored_into[std::string{args[1]}];
                    }
                }
            }
        }

        // Each slot and the field addresses computed from it, by the slot
        ankerl::unordered_dense::map<std::string, std::string> roots;
        for (const auto& seg : fn.segments) {
            for (const auto& inst : seg.instructions) {
                const auto [def, body]{split_definition(inst)};
                if (def.empty()) { continue; }
                if (body.starts_with("alloca ")) {
                    roots.insert_or_assign(std::string{def}, std::string{def});
                } else if (body.starts_with("get_element_ptr ")) {
                    const auto temps{mentioned_temps(body)};
                    const auto base{temps.empty() ? roots.end() : roots.find(temps.front())};
                    if (base != roots.end()) {
                        roots.insert_or_assign(std::string{def}, base->second);
                    }
                }
            }
        }

        ankerl::unordered_dense::set<std::string> read_roots;
        for (const auto& seg : fn.segments) {
            for (const auto& inst : seg.instructions) {
                const auto body{split_definition(inst).second};
                if (body.starts_with("alloca ")) { continue; }
                auto temps{mentioned_temps(body)};
                if (body.starts_with("get_element_ptr ") && !temps.empty()) {
                    temps.erase(temps.begin());
                } else if (body.starts_with("store ")) {
                    const auto args{store_args(body)};
                    if (args.size() == 2) { std::erase(temps, std::string{args[1]}); }
                }

                for (const auto& temp : temps) {
                    if (const auto found{roots.find(temp)}; found != roots.end()) {
                        read_roots.emplace(found->second);
                    }
                }
            }
        }

        ankerl::unordered_dense::set<std::string> dead_roots;
        for (const auto& [temp, root] : roots) {
            if (temp == root && !read_roots.contains(root)) { dead_roots.emplace(root); }
        }

        const auto is_dead{[&](std::string_view inst) {
            const auto [def, body]{split_definition(inst)};

            // A load, address or plain computation nothing reads computes nothing anyone sees
            if (!def.empty() && is_pure(body) && !uses.contains(std::string{def})) { return true; }
            // So does a temporary only ever written, with every write into it
            if (!def.empty() && dead_roots.contains(std::string{def})) { return true; }
            if (body.starts_with("get_element_ptr ")) {
                const auto temps{mentioned_temps(body)};
                if (!temps.empty() && dead_roots.contains(temps.front())) { return true; }
            }

            if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                const auto root{args.size() == 2 ? roots.find(std::string{args[1]}) : roots.end()};
                if (root != roots.end() && dead_roots.contains(root->second)) { return true; }
            }

            if (!def.empty() && body.starts_with("alloca ")) {
                const auto found{uses.find(std::string{def})};
                const auto stores{stored_into.find(std::string{def})};
                return found == uses.end() ||
                       (stores != stored_into.end() && stores->second == found->second);
            }

            if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                if (args.size() != 2 || !slots.contains(std::string{args[1]})) { return false; }
                const auto found{uses.find(std::string{args[1]})};
                const auto stores{stored_into.find(std::string{args[1]})};
                return found != uses.end() && stores != stored_into.end() &&
                       stores->second == found->second;
            }

            return false;
        }};
        for (auto& seg : fn.segments) {
            const auto before{seg.instructions.size()};
            std::erase_if(seg.instructions, is_dead);
            changed = changed || seg.instructions.size() != before;
        }
    }
}

// A slot that is only stored to and loaded from, where every store writes the same constant,
// reads as that constant
auto forward_constant_slots(function_dump& fn, known_map& known) -> bool {
    struct slot_use {
        stdx::option<std::string> value;
        bool                      forwardable{true};
    };
    drop_unreachable(fn);
    drop_overwritten_stores(fn);
    ankerl::unordered_dense::map<std::string, slot_use> slots;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (!def.empty() && body.starts_with("alloca ")) { slots[std::string{def}]; }
        }
    }

    if (slots.empty()) { return false; }

    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (body.starts_with("alloca ")) { continue; }
            if (!def.empty() && body.starts_with("load %")) { continue; }
            if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                if (args.size() == 2 && slots.contains(std::string{args[1]})) {
                    auto& use{slots[std::string{args[1]}]};
                    if (!is_constant_token(args[0]) || (use.value && *use.value != args[0])) {
                        use.forwardable = false;
                    } else {
                        use.value.emplace(args[0]);
                    }

                    if (slots.contains(std::string{args[0]})) {
                        slots[std::string{args[0]}].forwardable = false;
                    }

                    continue;
                }
            }

            DISCARD(rewrite_temps(body, [&](std::string_view t) -> std::string {
                if (const auto found{slots.find(std::string{t})}; found != slots.end()) {
                    found->second.forwardable = false;
                }

                return std::string{t};
            }));
        }
    }

    bool changed{false};
    for (auto& seg : fn.segments) {
        std::vector<std::string> kept;
        for (auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            const auto slot_of{[&](std::string_view name) -> stdx::option<const slot_use&> {
                const auto found{slots.find(std::string{name})};
                if (found == slots.end() || !found->second.forwardable || !found->second.value) {
                    return stdx::none;
                }

                return found->second;
            }};
            if (body.starts_with("alloca ") && slot_of(def)) {
                changed = true;
                continue;
            }

            if (body.starts_with("load %")) {
                if (const auto use{slot_of(stdx::string::substr(body, 5))}) {
                    known.insert_or_assign(std::string{def}, *use->value);
                    changed = true;
                    continue;
                }
            }

            if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                if (args.size() == 2 && slot_of(args[1])) {
                    changed = true;
                    continue;
                }
            }

            kept.emplace_back(std::move(inst));
        }

        seg.instructions = std::move(kept);
    }

    return changed;
}

// Folds all-constant instructions, constant branches, and constant slots until nothing changes
auto fold_constants(function_dump& fn) -> void {
    known_map known;
    for (bool changed{true}; changed;) {
        changed = false;
        for (auto& seg : fn.segments) {
            std::vector<std::string> kept;
            for (const auto& inst : seg.instructions) {
                const auto [def, body]{split_definition(inst)};
                auto substituted{rewrite_temps(body, [&](std::string_view t) -> std::string {
                    const auto found{known.find(std::string{t})};
                    return found == known.end() ? std::string{t} : found->second;
                })};
                if (!def.empty()) {
                    if (const auto in{parse_fold_input(substituted)}) {
                        if (const auto value{fold(*in)}) {
                            known.insert_or_assign(std::string{def}, *value);
                            changed = true;
                            continue;
                        }
                    }
                }

                if (substituted.starts_with("cond_goto true seg ") ||
                    substituted.starts_with("cond_goto false seg ")) {
                    const bool taken{substituted.starts_with("cond_goto true")};
                    const auto targets{
                        stdx::string::substr(substituted, substituted.find(" seg ") + 1)};
                    const auto comma{targets.find(", ")};
                    substituted = fmt::format("goto {}",
                                              taken ? stdx::string::substr(targets, 0, comma)
                                                    : stdx::string::substr(targets, comma + 2));
                    changed     = true;
                }

                kept.emplace_back(def.empty() ? substituted
                                              : fmt::format("{} = {}", def, substituted));
            }

            seg.instructions = std::move(kept);
        }

        changed = forward_constant_slots(fn, known) || changed;
    }
}

// An address computation, which reads no memory and so may sit anywhere before its first use
[[nodiscard]] auto is_address_computation(std::string_view body) -> bool {
    return body.starts_with("get_element_ptr ") || body.starts_with("global_addr ") ||
           body.starts_with("address_of ");
}

// A widening conversion, which reads no memory and can't fail
[[nodiscard]] auto is_pure_cast(std::string_view body) -> bool {
    return body.starts_with("widen_cast ");
}

[[nodiscard]] auto is_terminator_text(std::string_view body) -> bool {
    return body.starts_with("ret") || body.starts_with("goto ") || body.starts_with("cond_goto ") ||
           body.starts_with("unreachable");
}

// Whether an instruction may write memory or leave the segment
[[nodiscard]] auto may_write(std::string_view body) -> bool {
    return body.starts_with("store ") || body.starts_with("call ") ||
           body.starts_with("builtin_call ") || body.starts_with("inline_asm") ||
           body.starts_with("ret") || body.starts_with("goto ") || body.starts_with("cond_goto ") ||
           body.starts_with("unreachable");
}

// Slots only ever loaded from and stored to, whose address no other instruction sees
[[nodiscard]] auto private_slots(const function_dump& fn)
    -> ankerl::unordered_dense::set<std::string> {
    ankerl::unordered_dense::set<std::string> slots;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (!def.empty() && body.starts_with("alloca ")) { slots.emplace(def); }
        }
    }

    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (body.starts_with("alloca ")) { continue; }
            if (body.starts_with("load %")) { continue; }
            if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                if (args.size() == 2) { slots.erase(std::string{args[0]}); }
                continue;
            }

            for (const auto& temp : mentioned_temps(body)) { slots.erase(temp); }
        }
    }

    return slots;
}

// Slots whose address only reaches field addresses, loads, and store destinations
[[nodiscard]] auto private_aggregates(const function_dump& fn)
    -> ankerl::unordered_dense::map<std::string, std::string> {
    ankerl::unordered_dense::map<std::string, std::string> root;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (!def.empty() && body.starts_with("alloca ")) {
                root.insert_or_assign(std::string{def}, std::string{def});
            }
        }
    }

    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto [def, body]{split_definition(inst)};
            if (def.empty() || !body.starts_with("get_element_ptr ")) { continue; }
            const auto temps{mentioned_temps(body)};
            const auto base{temps.empty() ? root.end() : root.find(temps.front())};
            if (base != root.end()) { root.insert_or_assign(std::string{def}, base->second); }
        }
    }

    ankerl::unordered_dense::set<std::string> escaped;
    for (const auto& seg : fn.segments) {
        for (const auto& inst : seg.instructions) {
            const auto body{split_definition(inst).second};
            if (body.starts_with("alloca ") || body.starts_with("load %")) { continue; }
            auto temps{mentioned_temps(body)};
            if (body.starts_with("get_element_ptr ") && !temps.empty()) {
                temps.erase(temps.begin());
            } else if (body.starts_with("store ")) {
                const auto args{store_args(body)};
                if (args.size() == 2) { std::erase(temps, std::string{args[1]}); }
            }

            for (const auto& temp : temps) {
                if (const auto found{root.find(temp)}; found != root.end()) {
                    escaped.emplace(found->second);
                }
            }
        }
    }

    std::erase_if(root, [&](const auto& entry) { return escaped.contains(entry.second); });
    return root;
}

auto move_addresses_to_their_users(function_dump& fn) -> void {
    for (bool moved{true}; moved;) {
        moved = false;
        ankerl::unordered_dense::map<std::string, ankerl::unordered_dense::set<usize>> used_in;
        for (usize s{0}; s < fn.segments.size(); ++s) {
            for (const auto& inst : fn.segments[s].instructions) {
                const auto body{split_definition(inst).second};
                for (const auto& temp : mentioned_temps(body)) { used_in[temp].emplace(s); }
            }
        }

        for (usize s{0}; s < fn.segments.size() && !moved; ++s) {
            auto& insts{fn.segments[s].instructions};
            for (usize i{0}; i < insts.size(); ++i) {
                const auto [def, body]{split_definition(insts[i])};
                const auto users{used_in.find(std::string{def})};
                const bool movable{!def.empty() && body.starts_with("get_element_ptr ") &&
                                   users != used_in.end() && users->second.size() == 1 &&
                                   !users->second.contains(s)};
                if (!movable) { continue; }

                auto& target{fn.segments[*users->second.begin()].instructions};
                target.insert(target.begin(), std::move(insts[i]));
                insts.erase(insts.begin() + static_cast<idiff>(i));
                moved = true;
                break;
            }
        }
    }
}

// Places each address computation and loads with no write before its use, just before the
// first instruction of its segment that uses it
auto sink_address_computations(function_dump& fn) -> void {
    const auto slots{private_slots(fn)};
    const auto aggregates{private_aggregates(fn)};

    // The slot a store's destination belongs to, when the store may wait for its next use
    const auto waiting_slot{[&](std::string_view body) -> stdx::option<std::string> {
        if (!body.starts_with("store ")) { return stdx::none; }
        const auto args{store_args(body)};
        if (args.size() != 2) { return stdx::none; }
        const std::string dest{args[1]};

        // A parameter never changes, so its spill may wait for the slot's use like a constant's
        const bool fixed_value{is_constant_token(args[0]) || args[0].starts_with("param.")};
        if (fixed_value && slots.contains(dest)) { return dest; }
        const auto found{aggregates.find(dest)};
        if (found != aggregates.end() && (found->second != dest || fixed_value)) {
            return found->second;
        }

        return stdx::none;
    }};
    const auto slot_of{[&](const std::string& temp) -> stdx::option<std::string> {
        if (slots.contains(temp)) { return temp; }
        const auto found{aggregates.find(temp)};
        if (found != aggregates.end()) { return found->second; }
        return stdx::none;
    }};
    for (auto& seg : fn.segments) {
        auto&      insts{seg.instructions};
        const auto n{insts.size()};

        // What each instruction defines and reads, and where each temporary is next read
        std::vector<std::string>                                      defs(n);
        std::vector<std::vector<std::string>>                         reads(n);
        std::vector<usize>                                            writes_before(n + 1, 0);
        ankerl::unordered_dense::map<std::string, std::vector<usize>> read_at;
        for (usize i{0}; i < n; ++i) {
            const auto [def, body]{split_definition(insts[i])};
            defs[i]  = std::string{def};
            reads[i] = mentioned_temps(body);
            for (const auto& temp : reads[i]) { read_at[temp].emplace_back(i); }
            writes_before[i + 1] = writes_before[i] + (may_write(body) ? 1 : 0);
        }

        const auto next_read{[&](const std::string& temp, usize after) -> stdx::option<usize> {
            const auto found{read_at.find(temp)};
            if (found == read_at.end()) { return stdx::none; }
            const auto at{std::ranges::upper_bound(found->second, after)};
            if (at == found->second.end()) { return stdx::none; }
            return *at;
        }};

        // Whether every store between a load at `from` and its use at `to` writes another private
        // slot, whose address never escapes, so it can't change what the load reads
        const auto only_other_writes{[&](usize from, usize to) {
            const auto address{reads[from].empty() ? stdx::none : slot_of(reads[from].front())};
            for (usize k{from + 1}; k < to; ++k) {
                const auto body{split_definition(insts[k]).second};
                if (!body.starts_with("store ")) { continue; }
                const auto args{store_args(body)};
                if (args.size() != 2) { return false; }
                const auto written{slot_of(std::string{args[1]})};
                if (!written || written == address) { return false; }
            }

            return true;
        }};
        ankerl::unordered_dense::map<std::string, usize> deferred;

        // Each deferred constant store, by the slot it writes
        ankerl::unordered_dense::map<std::string, std::vector<usize>> stores;
        bool                                                          any_store{false};
        for (usize i{0}; i < n; ++i) {
            const auto body{split_definition(insts[i]).second};
            if (body.starts_with("store ")) {
                if (const auto slot{waiting_slot(body)}) {
                    stores[*slot].emplace_back(i);
                    any_store = true;
                }

                continue;
            }

            const bool is_load{body.starts_with("load ")};
            if (defs[i].empty() ||
                (!is_address_computation(body) && !is_load && !is_pure_cast(body))) {
                continue;
            }

            const auto use{next_read(defs[i], i)};
            if (!use) { continue; }
            // A read may not move past a write
            if (is_load && writes_before[*use] != writes_before[i + 1] &&
                !only_other_writes(i, *use)) {
                continue;
            }

            deferred.insert_or_assign(defs[i], i);
        }

        if (deferred.empty() && !any_store) { continue; }

        std::vector<std::string> out;
        out.reserve(n);
        std::vector<bool> placed(n, false);
        std::vector<bool> waiting(n, false);
        for (const auto& [slot, at] : stores) {
            for (const auto i : at) { waiting[i] = true; }
        }

        // Started before it is emitted, so a store waiting on a slot isn't placed twice
        std::vector<bool> started(n, false);
        const auto        emit{[&](usize at) {
            placed[at] = true;
            out.emplace_back(std::move(insts[at]));
        }};
        const auto        place{[&](this const auto& self, usize at) -> void {
            started[at] = true;
            for (const auto& temp : reads[at]) {
                const auto found{deferred.find(temp)};
                if (found != deferred.end() && !started[found->second]) { self(found->second); }
            }

            // A use of a slot lets the stores waiting on it go first
            const auto body{split_definition(insts[at]).second};
            if (!waiting[at] && !is_address_computation(body)) {
                for (const auto& temp : reads[at]) {
                    const auto slot{slot_of(temp)};
                    const auto found{slot ? stores.find(*slot) : stores.end()};
                    if (found == stores.end()) { continue; }
                    // Only the stores that came before this use
                    for (const auto i : found->second) {
                        if (!started[i] && i < at) { self(i); }
                    }
                }
            }

            if (is_terminator_text(body)) {
                for (usize k{0}; k < n; ++k) {
                    if (waiting[k] && !started[k]) { self(k); }
                }
            }

            emit(at);
        }};
        for (usize i{0}; i < n; ++i) {
            if (started[i] || waiting[i]) { continue; }
            if (!defs[i].empty() && deferred.contains(defs[i])) { continue; }
            place(i);
        }

        for (usize k{0}; k < n; ++k) {
            if (waiting[k] && !started[k]) { place(k); }
        }

        insts = std::move(out);
    }
}

[[nodiscard]] auto parse_seg_ref(std::string_view text) -> stdx::option<u32> {
    if (!text.starts_with("seg ")) { return stdx::none; }
    return parse_u32(stdx::string::substr(text, 4));
}

// The segments a terminator jumps to, in operand order
[[nodiscard]] auto successors(std::string_view terminator) -> std::vector<u32> {
    std::vector<u32> out;
    if (terminator.starts_with("goto ")) {
        if (const auto s{parse_seg_ref(stdx::string::substr(terminator, 5))}) {
            out.emplace_back(*s);
        }
    } else if (terminator.starts_with("cond_goto ")) {
        const auto first{terminator.find(" seg ")};
        if (first != std::string_view::npos) {
            for (const auto part :
                 string_utils::split(stdx::string::substr(terminator, first + 1), ", ")) {
                if (const auto s{parse_seg_ref(part)}) { out.emplace_back(*s); }
            }
        }
    }

    return out;
}

// Rewrites each `seg N` reference through `f`
template <typename F> [[nodiscard]] auto rewrite_segs(std::string_view text, F&& f) -> std::string {
    std::string out;
    for (usize i{0}; i < text.size();) {
        if (stdx::string::substr(text, i).starts_with("seg ")) {
            usize end{i + 4};
            while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; }
            if (const auto id{parse_u32(stdx::string::substr(text, i + 4, end - i - 4))}) {
                out += fmt::format("seg {}", f(*id));
                i = end;
                continue;
            }
        }

        out += text[i++];
    }

    return out;
}

auto canonicalize(function_dump& fn) -> void {
    if (fn.segments.empty()) { return; }
    ankerl::unordered_dense::map<u32, usize> by_id;
    for (usize i{0}; i < fn.segments.size(); ++i) { by_id.insert_or_assign(fn.segments[i].id, i); }

    // A segment that only jumps on is threaded through
    const auto forward{[&](u32 id) {
        for (usize hops{0}; hops < fn.segments.size(); ++hops) {
            const auto found{by_id.find(id)};
            if (found == by_id.end()) { break; }
            const auto& insts{fn.segments[found->second].instructions};
            if (insts.size() != 1 || !insts[0].starts_with("goto ")) { break; }
            const auto next{parse_seg_ref(stdx::string::substr(insts[0], 5))};
            if (!next || *next == id) { break; }
            id = *next;
        }

        return id;
    }};

    // The reachable segments, with every jump already threaded
    using body_map = ankerl::unordered_dense::map<u32, std::vector<std::string>>;
    body_map   bodies;
    const auto entry{forward(fn.segments.front().id)};
    for (std::vector<u32> stack{entry}; !stack.empty();) {
        const auto id{stack.back()};
        stack.pop_back();
        if (bodies.contains(id) || !by_id.contains(id)) { continue; }
        auto& insts{bodies[id]};
        for (const auto& inst : fn.segments[by_id[id]].instructions) {
            auto threaded{rewrite_segs(inst, forward)};

            // A branch whose arms reach one segment is a jump there
            if (threaded.starts_with("cond_goto ")) {
                const auto targets{successors(threaded)};
                if (targets.size() == 2 && targets[0] == targets[1]) {
                    threaded = fmt::format("goto seg {}", targets[0]);
                }
            }

            insts.emplace_back(std::move(threaded));
        }

        if (insts.empty()) { continue; }
        for (const auto s : successors(insts.back())) { stack.emplace_back(s); }
    }

    // A segment only one jump reaches joins the segment that jumps to it
    ankerl::unordered_dense::map<u32, u32> preds;
    for (const auto& [id, insts] : bodies) {
        if (insts.empty()) { continue; }
        for (const auto s : successors(insts.back())) { preds[s] += 1; }
    }

    ankerl::unordered_dense::set<u32> merged;
    for (auto& [id, insts] : bodies) {
        if (merged.contains(id)) { continue; }
        while (!insts.empty() && insts.back().starts_with("goto ")) {
            const auto next{*parse_seg_ref(stdx::string::substr(insts.back(), 5))};
            if (next == entry || next == id || preds[next] != 1 || merged.contains(next)) { break; }
            insts.pop_back();
            const auto taken{std::move(bodies[next])};
            insts.insert(insts.end(), taken.begin(), taken.end());
            merged.insert(next);
        }
    }

    std::vector<u32>                       order;
    ankerl::unordered_dense::map<u32, u32> renumbered;
    for (std::vector<u32> stack{entry}; !stack.empty();) {
        const auto id{stack.back()};
        stack.pop_back();
        if (renumbered.contains(id) || merged.contains(id) || !bodies.contains(id)) { continue; }
        renumbered.insert_or_assign(id, static_cast<u32>(order.size()));
        order.emplace_back(id);
        const auto& insts{bodies[id]};
        if (insts.empty()) { continue; }
        auto next{successors(insts.back())};
        std::ranges::reverse(next);
        for (const auto s : next) { stack.emplace_back(s); }
    }

    std::vector<std::string>  allocas;
    std::vector<segment_dump> segments;
    for (const auto id : order) {
        segment_dump seg{.id = renumbered[id], .instructions = {}};
        for (const auto& inst : bodies[id]) {
            auto rewritten{rewrite_segs(inst, [&](u32 s) { return renumbered[s]; })};
            if (split_definition(rewritten).second.starts_with("alloca ")) {
                allocas.emplace_back(std::move(rewritten));
            } else {
                seg.instructions.emplace_back(std::move(rewritten));
            }
        }

        segments.emplace_back(std::move(seg));
    }

    // Slots come first numbered by their first use
    ankerl::unordered_dense::map<std::string, std::string> temps;
    const auto                                             number{[&](std::string_view t) {
        const auto [it,
                    inserted]{temps.try_emplace(std::string{t}, fmt::format("%{}", temps.size()))};
        return it->second;
    }};
    ankerl::unordered_dense::set<std::string>              slot_names;
    for (const auto& slot : allocas) { slot_names.emplace(split_definition(slot).first); }
    for (const auto& seg : segments) {
        for (const auto& inst : seg.instructions) {
            for (const auto& temp : mentioned_temps(inst)) {
                if (slot_names.contains(temp)) { DISCARD(number(temp)); }
            }
        }
    }

    for (auto& slot : allocas) { slot = rewrite_temps(slot, number); }
    for (auto& seg : segments) {
        for (auto& inst : seg.instructions) { inst = rewrite_temps(inst, number); }
    }

    std::ranges::sort(allocas, {}, [](const std::string& slot) {
        return parse_u32(stdx::string::substr(slot, 1, slot.find(' ') - 1)).value_or(0);
    });
    segments.front().instructions.insert(
        segments.front().instructions.begin(), allocas.begin(), allocas.end());
    fn.segments = std::move(segments);
}

// The function name in a header, which orders the functions
[[nodiscard]] auto sort_key(const function_dump& fn) -> std::string_view { return fn.header; }

} // namespace

auto normalize_gir(std::string_view dump) -> std::string {
    auto mod{parse(erase_nominal_discriminators(joined_string_lines(dump)))};
    for (auto& fn : mod.functions) {
        for (auto& seg : fn.segments) {
            for (auto& inst : seg.instructions) {
                inst = without_value_volatility(as_direct_call(without_address_mutability(inst)));

                // Returning a `void` value returns nothing, and a `noreturn` one is never reached
                if (inst == "ret void void") { inst = "ret void"; }
                if (inst.starts_with("ret noreturn ")) { inst = "unreachable"; }
            }
        }
        drop_identity_casts(fn);

        // Merging segments exposes more to fold, so both passes run again on the merged form
        for (usize round{0}; round < 2; ++round) {
            fold_constants(fn);
            drop_dead_slots(fn);
            move_addresses_to_their_users(fn);
            sink_address_computations(fn);
            canonicalize(fn);
        }
    }

    std::ranges::sort(mod.items);

    // Functions whose erased symbols match are ordered by their bodies, local functions by
    // their bodies alone
    std::vector<std::pair<std::string, std::string>> rendered;
    for (const auto& fn : mod.functions) {
        std::string text{fmt::format("{}\n", fn.header)};
        for (const auto& seg : fn.segments) {
            text += fmt::format("  seg {}:\n", seg.id);
            for (const auto& inst : seg.instructions) { text += fmt::format("    {}\n", inst); }
        }

        text += "\n";
        rendered.emplace_back(
            fmt::format("{}\n{}", without_local_numbers(sort_key(fn)), without_local_numbers(text)),
            std::move(text));
    }

    std::ranges::sort(rendered);
    std::string functions;
    for (const auto& [key, text] : rendered) { functions += text; }

    // Hidden literal globals are numbered in the order Sema reaches them, so both sides are
    // renamed by where the sorted functions first use them
    ankerl::unordered_dense::map<std::string, std::string> renamed;
    const auto rename{[&](const std::string& text, bool assign, auto find, auto spell) {
        std::string out;
        usize       at{0};
        while (at < text.size()) {
            const auto found{find(text, at)};
            if (!found) {
                out += text.substr(at);
                break;
            }

            const auto [start, len]{*found};
            out += text.substr(at, start - at);
            const auto name{text.substr(start, len)};
            if (assign && !renamed.contains(name)) { renamed.emplace(name, spell(name)); }

            const auto it{renamed.find(name)};
            out += it == renamed.end() ? name : it->second;
            at = start + len;
        }

        return out;
    }};
    // A monomorph's symbol spells a local function's instance suffix with an `x`, not a `.`
    const auto canonical{[](const std::string& text) {
        std::string out{text};
        usize       at{0};
        while (const auto found{local_function_at(out, at)}) {
            const auto [start, len]{*found};
            const auto digits{
                out.find_first_not_of("0123456789", out.find_first_of("0123456789", start))};
            if (digits < start + len && out[digits] == 'x') { out[digits] = '.'; }
            at = start + len;
        }

        return out;
    }};
    functions = canonical(functions);
    const auto literal{[&](std::string_view) { return fmt::format(".lit#{}", renamed.size()); }};
    const auto local{[&](std::string_view name) {
        return fmt::format(
            "{}{}", name.starts_with("closure") ? "closure#" : "localfn.#", renamed.size());
    }};
    functions = rename(functions, true, literal_global_at, literal);
    functions = rename(functions, true, local_function_at, local);
    for (auto& item : mod.items) {
        item = rename(
            rename(item, false, literal_global_at, literal), false, local_function_at, local);
    }
    std::ranges::sort(mod.items);

    std::string out;
    for (const auto& item : mod.items) { out += fmt::format("{}\n", item); }
    return out + functions;
}

} // namespace ghoti::tests::helpers
