#include "helpers/gir_normalize.hh"

#include <algorithm>
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

#include "support/int128.hh"

namespace ghoti::tests::helpers {

namespace {

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

[[nodiscard]] auto split_lines(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end{text.find('\n')};
        lines.emplace_back(stdx::string::substr(text, 0, end));
        if (end == std::string_view::npos) { break; }
        text.remove_prefix(end + 1);
    }
    return lines;
}

[[nodiscard]] auto parse_u32(std::string_view text) -> stdx::option<u32> {
    u32 value{0};
    const auto [ptr, ec]{std::from_chars(text.data(), text.data() + text.size(), value)};
    if (ec != std::errc{} || ptr != text.data() + text.size()) { return stdx::none; }
    return value;
}

[[nodiscard]] auto parse(std::string_view dump) -> module_dump {
    module_dump out;
    for (const auto line : split_lines(dump)) {
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

[[nodiscard]] auto split_operands(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const auto comma{text.find(", ")};
        out.emplace_back(stdx::string::substr(text, 0, comma));
        if (comma == std::string_view::npos) { break; }
        text.remove_prefix(comma + 2);
    }
    return out;
}

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
    in.operands = split_operands(stdx::string::substr(rest, type_end + 1));
    return in;
}

[[nodiscard]] auto is_bool_constant(std::string_view v) -> bool {
    return v == "true" || v == "false";
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

    const auto shape{int_shape_of(in.type)};
    if (!shape) { return stdx::none; }
    if (ops.size() == 1) {
        const auto v{parse_int(ops[0])};
        if (!v) { return stdx::none; }
        if (in.kind == "widen_cast") { return format_int(*v); }
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
                const auto args{split_operands(stdx::string::substr(inst, 6))};
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

// A load nothing reads, and then a slot only ever stored to, as an expression statement's unused
// result slot leaves behind
auto drop_dead_slots(function_dump& fn) -> void {
    bool changed{true};
    while (changed) {
        changed = false;
        ankerl::unordered_dense::map<std::string, usize> uses;
        ankerl::unordered_dense::map<std::string, usize> stored_into;
        for (const auto& seg : fn.segments) {
            for (const auto& inst : seg.instructions) {
                const auto [def, body]{split_definition(inst)};
                DISCARD(rewrite_temps(body, [&](std::string_view t) -> std::string {
                    ++uses[std::string{t}];
                    return std::string{t};
                }));
                if (body.starts_with("store ")) {
                    const auto args{split_operands(stdx::string::substr(body, 6))};
                    if (args.size() == 2 && args[1].starts_with('%') && args[0] != args[1]) {
                        ++stored_into[std::string{args[1]}];
                    }
                }
            }
        }
        const auto is_dead{[&](std::string_view inst) {
            const auto [def, body]{split_definition(inst)};
            if (!def.empty() && body.starts_with("load ") && !uses.contains(std::string{def})) {
                return true;
            }
            if (!def.empty() && body.starts_with("alloca ")) {
                const auto found{uses.find(std::string{def})};
                const auto stores{stored_into.find(std::string{def})};
                return found == uses.end() ||
                       (stores != stored_into.end() && stores->second == found->second);
            }
            if (body.starts_with("store ")) {
                const auto args{split_operands(stdx::string::substr(body, 6))};
                if (args.size() != 2 || !args[1].starts_with('%')) { return false; }
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
                const auto args{split_operands(stdx::string::substr(body, 6))};
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
                const auto args{split_operands(stdx::string::substr(body, 6))};
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
            for (const auto part : split_operands(stdx::string::substr(terminator, first + 1))) {
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
            insts.emplace_back(rewrite_segs(inst, forward));
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
    segments.front().instructions.insert(
        segments.front().instructions.begin(), allocas.begin(), allocas.end());

    // Temporaries take numbers in order of first appearance
    ankerl::unordered_dense::map<std::string, std::string> temps;
    for (auto& seg : segments) {
        for (auto& inst : seg.instructions) {
            inst = rewrite_temps(inst, [&](std::string_view t) -> std::string {
                const auto [it, inserted]{
                    temps.try_emplace(std::string{t}, fmt::format("%{}", temps.size()))};
                return it->second;
            });
        }
    }
    fn.segments = std::move(segments);
}

// The function name in a header, which orders the functions
[[nodiscard]] auto sort_key(const function_dump& fn) -> std::string_view { return fn.header; }

} // namespace

auto normalize_gir(std::string_view dump) -> std::string {
    auto mod{parse(dump)};
    for (auto& fn : mod.functions) {
        // Merging segments exposes more to fold, so both passes run again on the merged form
        for (usize round{0}; round < 2; ++round) {
            fold_constants(fn);
            drop_dead_slots(fn);
            canonicalize(fn);
        }
    }
    std::ranges::sort(mod.items);
    std::ranges::sort(mod.functions, {}, sort_key);

    std::string out;
    for (const auto& item : mod.items) { out += fmt::format("{}\n", item); }
    for (const auto& fn : mod.functions) {
        out += fmt::format("{}\n", fn.header);
        for (const auto& seg : fn.segments) {
            out += fmt::format("  seg {}:\n", seg.id);
            for (const auto& inst : seg.instructions) { out += fmt::format("    {}\n", inst); }
        }
        out += "\n";
    }
    return out;
}

} // namespace ghoti::tests::helpers
