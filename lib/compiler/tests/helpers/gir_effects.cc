#include "helpers/gir_effects.hh"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

namespace ghoti::tests::helpers {

namespace {

constexpr std::string_view INDENT{"    "};

// The operators that compute a value and touch nothing, so where they run doesn't matter
constexpr std::array PURE_OPS{
    std::string_view{"alloca"},
    std::string_view{"load"},
    std::string_view{"get_element_ptr"},
    std::string_view{"address_of"},
    std::string_view{"global_addr"},
    std::string_view{"add"},
    std::string_view{"sub"},
    std::string_view{"mul"},
    std::string_view{"div"},
    std::string_view{"rem"},
    std::string_view{"shl"},
    std::string_view{"shr"},
    std::string_view{"and"},
    std::string_view{"or"},
    std::string_view{"xor"},
    std::string_view{"not"},
    std::string_view{"neg"},
    std::string_view{"eq"},
    std::string_view{"ne"},
    std::string_view{"lt"},
    std::string_view{"le"},
    std::string_view{"gt"},
    std::string_view{"ge"},
    std::string_view{"int_cast"},
    std::string_view{"widen_cast"},
    std::string_view{"trunc"},
    std::string_view{"bit_cast"},
    std::string_view{"float_cast"},
    std::string_view{"ptr_cast"},
};

struct instruction {
    std::string_view def;
    std::string_view text;
    std::string_view op;
    u32              segment;
    bool             effect;
    // The temporaries the text names, in order
    std::vector<usize> operands;
};

struct function_text {
    std::vector<bool>                                     pointer_params;
    std::vector<instruction>                              lines;
    std::vector<u32>                                      segments;
    ankerl::unordered_dense::map<std::string_view, usize> defined;
};

[[nodiscard]] auto opcode_of(std::string_view text) -> std::string_view {
    const auto op{text.substr(0, text.find(' '))};
    return op.substr(0, op.find('.'));
}

// Each `%N` outside a string literal, with where it starts and ends
template <typename F> void for_each_temp(std::string_view text, F&& f) {
    bool quoted{false};
    for (usize i{0}; i < text.size(); ++i) {
        const char c{text[i]};
        if (quoted) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                quoted = false;
            }
            continue;
        }

        if (c == '"') {
            quoted = true;
            continue;
        }

        if (c != '%') { continue; }
        usize end{i + 1};
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') { ++end; }
        if (end > i + 1) {
            f(i, end);
            i = end - 1;
        }
    }
}

[[nodiscard]] auto parse_params(std::string_view header) -> std::vector<bool> {
    std::vector<bool> out;
    const auto        open{header.find('(')};
    if (open == std::string_view::npos) { return out; }
    int   depth{0};
    usize start{open + 1};
    for (usize i{open}; i < header.size(); ++i) {
        const char c{header[i]};
        if (c == '(') { ++depth; }
        if (c == ')') { --depth; }
        const bool last{depth == 0 && c == ')'};
        if ((depth == 1 && c == ',') || last) {
            const auto param{stdx::string::trim(header.substr(start, i - start))};
            const auto colon{param.find(": ")};
            if (colon != std::string_view::npos) {
                const auto type{param.substr(colon + 2)};
                out.emplace_back(type.starts_with('^') || type.starts_with('&'));
            }
            start = i + 1;
        }
        if (last) { break; }
    }
    return out;
}

[[nodiscard]] auto parse(std::string_view text) -> function_text {
    function_text out;
    u32           segment{0};
    bool          header{true};
    for (usize at{0}; at < text.size();) {
        const auto end{std::min(text.find('\n', at), text.size())};
        const auto line{text.substr(at, end - at)};
        at = end + 1;
        if (header) {
            out.pointer_params = parse_params(line);
            header             = false;
            continue;
        }

        if (line.starts_with("  seg ")) {
            const auto digits{line.substr(6, line.find(':') - 6)};
            segment = static_cast<u32>(std::stoul(std::string{digits}));
            out.segments.emplace_back(segment);
            continue;
        }

        if (!line.starts_with(INDENT)) { continue; }
        auto             body{line.substr(INDENT.size())};
        std::string_view def;
        if (body.starts_with('%')) {
            const auto eq{body.find(" = ")};
            if (eq != std::string_view::npos) {
                def  = body.substr(0, eq);
                body = body.substr(eq + 3);
            }
        }

        const auto op{opcode_of(body)};
        const bool pure{!def.empty() && std::ranges::contains(PURE_OPS, op)};
        out.lines.emplace_back(instruction{.def      = def,
                                           .text     = body,
                                           .op       = op,
                                           .segment  = segment,
                                           .effect   = !pure,
                                           .operands = {}});
        if (!def.empty()) { out.defined.insert_or_assign(def, out.lines.size() - 1); }
    }

    for (auto& line : out.lines) {
        for_each_temp(line.text, [&](usize from, usize to) {
            const auto found{out.defined.find(line.text.substr(from, to - from))};
            if (found != out.defined.end()) { line.operands.emplace_back(found->second); }
        });
    }
    return out;
}

// Distinct expression texts by number, so a tree names its children by id, not by copy
class interner {
  public:
    [[nodiscard]] auto id(const std::string& text) -> u32 {
        const auto [it, fresh]{ids_.try_emplace(text, static_cast<u32>(texts_.size()))};
        if (fresh) { texts_.emplace_back(text); }
        return it->second;
    }

    [[nodiscard]] auto text(u32 id) const -> const std::string& { return texts_[id]; }

  private:
    ankerl::unordered_dense::map<std::string, u32> ids_;
    std::vector<std::string>                       texts_;
};

// A function's values as expression trees, built twice: first without what loads read, to key
// the stores and decide which loads are forwarded, then with it. The second trees share
// `shared` with the function compared against
class canonicalizer {
  public:
    canonicalizer(function_text fn, interner& shared) : fn_{std::move(fn)}, shared_{shared} {
        const auto n{fn_.lines.size()};
        raw_.resize(n);
        final_.resize(n);
        alloca_ids_.resize(n);
        users_.resize(n);
        ankerl::unordered_dense::map<std::string, u32> allocas;
        for (usize i{0}; i < n; ++i) {
            const auto& line{fn_.lines[i]};
            if (line.op == "alloca") { alloca_ids_[i] = allocas[std::string{line.text}]++; }
            for (const auto operand : line.operands) { users_[operand].emplace_back(i); }
        }

        for (usize i{0}; i < n; ++i) {
            const auto& line{fn_.lines[i]};
            if (line.op != "store") { continue; }
            const auto split{line.text.rfind(", ")};
            if (split == std::string_view::npos) { continue; }
            const auto value{line.text.substr(6, split - 6)};
            auto       target{address_operand(i) ? raw_tree(line.operands.back())
                                                 : std::string{line.text.substr(split + 2)}};
            stores_[std::move(target)].emplace_back(store_site{.line = i, .value = value});
            if (address_operand(i)) {
                if (const auto root{alloca_root(line.operands.back())}) {
                    root_writes_[*root].emplace_back(i);
                }
            }
        }

        find_scratch();
        find_built();

        // How many live effects come before each line in its segment
        before_.resize(n);
        ankerl::unordered_dense::map<u32, usize> seen;
        for (usize i{0}; i < n; ++i) {
            const auto& line{fn_.lines[i]};
            auto&       count{seen[line.segment]};
            before_[i] = count;
            if (line.effect && !dead_store(i)) { ++count; }
        }
    }

    [[nodiscard]] auto effects() -> std::vector<std::pair<u32, std::string>> {
        std::vector<std::pair<u32, std::string>> out;
        for (usize i{0}; i < fn_.lines.size(); ++i) {
            const auto& line{fn_.lines[i]};
            if (!line.effect || dead_store(i)) { continue; }
            out.emplace_back(line.segment, canonical(i));
        }
        return out;
    }

    [[nodiscard]] auto segments() const -> const std::vector<u32>& { return fn_.segments; }

  private:
    struct store_site {
        usize            line;
        std::string_view value;
    };

    [[nodiscard]] auto raw_id(usize i) -> u32 { return raw_names_.id(raw_tree(i)); }
    [[nodiscard]] auto final_id(usize i) -> u32 { return shared_.id(canonical(i)); }

    [[nodiscard]] auto expand(std::string_view text, bool raw) -> std::string {
        std::string out;
        usize       last{0};
        for_each_temp(text, [&](usize from, usize to) {
            out += text.substr(last, from - last);
            const auto found{fn_.defined.find(text.substr(from, to - from))};
            if (found == fn_.defined.end()) {
                out += text.substr(from, to - from);
            } else {
                const auto child{found->second};
                const auto id{raw ? raw_id(child) : final_id(child)};
                // A forwarded constant or a by-value parameter stands for itself
                if (bare_.contains(child)) {
                    out += raw ? raw_names_.text(id) : shared_.text(id);
                } else {
                    out += fmt::format("{{#{}}}", id);
                }
            }
            last = to;
        });
        out += text.substr(last);
        return out;
    }

    // A load of a parameter passed by value reads the parameter itself
    [[nodiscard]] auto by_value_param(std::string_view address) const -> bool {
        if (!address.starts_with("param.")) { return false; }
        const auto index{std::stoul(std::string{address.substr(6)})};
        return index < fn_.pointer_params.size() && !fn_.pointer_params[index];
    }

    [[nodiscard]] auto raw_tree(usize i) -> const std::string& {
        if (raw_[i]) { return *raw_[i]; }
        const auto& line{fn_.lines[i]};
        std::string out;
        if (line.op == "alloca") {
            out = fmt::format("{}#{}", line.text, alloca_ids_[i]);
        } else if (line.op == "load" && by_value_param(line.text.substr(5))) {
            out = std::string{line.text.substr(5)};
            bare_.insert(i);
        } else {
            out = expand(line.text, true);
        }
        raw_[i] = std::move(out);
        return *raw_[i];
    }

    // The `alloca` an address is an offset into, when its chain is all offsets
    [[nodiscard]] auto alloca_root(usize i) const -> stdx::option<usize> {
        const auto& line{fn_.lines[i]};
        if (line.op == "alloca") { return i; }
        if (line.op == "get_element_ptr" && !line.operands.empty()) {
            return alloca_root(line.operands.front());
        }
        return stdx::none;
    }

    [[nodiscard]] static auto constant(std::string_view value) -> bool {
        return value.find('%') == std::string_view::npos &&
               value.find("param.") == std::string_view::npos && !value.starts_with('@');
    }

    // The constant a load of a scratch slot reads, following whole copies between slots
    [[nodiscard]] auto forwarded(const std::string& address) -> stdx::option<std::string> {
        const auto found{stores_.find(address)};
        if (found != stores_.end() && found->second.size() == 1 &&
            constant(found->second.front().value)) {
            return std::string{found->second.front().value};
        }

        // `gep T {slot}, k` of a slot written once with a whole copy of another slot
        constexpr std::string_view GEP{"get_element_ptr "};
        if (!address.starts_with(GEP)) { return stdx::none; }
        const auto open{address.find(" {#")};
        if (open == std::string::npos) { return stdx::none; }
        const auto close{address.find('}', open)};
        if (close == std::string::npos) { return stdx::none; }
        const auto base_id{
            static_cast<u32>(std::stoul(address.substr(open + 3, close - open - 3)))};
        const auto whole{stores_.find(raw_names_.text(base_id))};
        if (whole == stores_.end() || whole->second.size() != 1) { return stdx::none; }
        const auto copied{fn_.defined.find(whole->second.front().value)};
        if (copied == fn_.defined.end() || fn_.lines[copied->second].op != "load") {
            return stdx::none;
        }

        const auto& source_line{fn_.lines[copied->second]};
        if (source_line.operands.empty() || !alloca_root(source_line.operands.front())) {
            return stdx::none;
        }

        const auto source{raw_id(source_line.operands.front())};
        return forwarded(fmt::format(
            "{}{{#{}}}{}", address.substr(0, open + 1), source, address.substr(close + 1)));
    }

    // The value a slot holds when its one store, at entry, writes it and nothing else writes or
    // addresses the slot
    [[nodiscard]] auto spilled_value(usize slot) -> stdx::option<std::string> {
        if (!held_.contains(slot)) { return stdx::none; }
        const auto found{stores_.find(raw_tree(slot))};
        if (found == stores_.end() || found->second.size() != 1) { return stdx::none; }
        const auto& store{found->second.front()};

        // The value stored, whatever computed it
        if (const auto def{fn_.defined.find(store.value)}; def != fn_.defined.end()) {
            if (def->second == slot) { return stdx::none; }
            return bare_.contains(def->second) ? canonical(def->second)
                                               : fmt::format("{{#{}}}", final_id(def->second));
        }
        return std::string{store.value};
    }

    // A slot whose only uses are the stores building it and loads forwarded out of it
    void find_scratch() {
        ankerl::unordered_dense::set<usize> candidates;
        for (usize i{0}; i < fn_.lines.size(); ++i) {
            if (fn_.lines[i].op == "alloca") { candidates.insert(i); }
        }

        bool changed{true};
        while (changed) {
            changed = false;
            for (usize i{0}; i < fn_.lines.size(); ++i) {
                const auto& line{fn_.lines[i]};
                for (usize k{0}; k < line.operands.size(); ++k) {
                    const auto root{alloca_root(line.operands[k])};
                    if (!root || !candidates.contains(*root)) { continue; }
                    if (allowed_use(i, k, candidates)) { continue; }
                    candidates.erase(*root);
                    changed = true;
                }
            }
        }
        scratch_ = std::move(candidates);
    }

    [[nodiscard]] auto
    allowed_use(usize user, usize k, const ankerl::unordered_dense::set<usize>& scratch) -> bool {
        const auto& line{fn_.lines[user]};
        if (line.op == "get_element_ptr" && k == 0) { return true; }
        if (line.op == "store") { return k + 1 == line.operands.size() && address_operand(user); }
        if (line.op != "load") { return false; }
        if (forwarded(raw_tree(line.operands.front()))) { return true; }

        // A whole copy read only to fill another scratch slot
        for (const auto j : users_[user]) {
            const auto& other{fn_.lines[j]};
            if (other.op != "store" || other.operands.size() != 2 || other.operands[0] != user) {
                return false;
            }
            const auto target{alloca_root(other.operands[1])};
            if (!target || !scratch.contains(*target)) { return false; }
        }
        return true;
    }

    // Whether a store's last temporary is its destination, not part of its value
    [[nodiscard]] auto address_operand(usize i) const -> bool {
        const auto& line{fn_.lines[i]};
        const auto  split{line.text.rfind(", ")};
        return split != std::string_view::npos && line.text.substr(split + 2).starts_with('%');
    }

    [[nodiscard]] auto dead_store(usize i) const -> bool {
        const auto& line{fn_.lines[i]};
        if (line.op != "store" || line.operands.empty() || !address_operand(i)) { return false; }
        const auto root{alloca_root(line.operands.back())};
        return root &&
               (scratch_.contains(*root) || built_.contains(*root) || held_.contains(*root));
    }

    // A slot each of whose fields is stored once and which is only read whole, so where its
    // fields were stored doesn't matter, only what they hold
    void find_built() {
        for (usize i{0}; i < fn_.lines.size(); ++i) {
            if (fn_.lines[i].op != "alloca" || scratch_.contains(i)) { continue; }
            bool                                      ok{!users_[i].empty()};
            bool                                      read{false};
            ankerl::unordered_dense::set<std::string> fields;
            for (const auto user : users_[i]) {
                const auto& line{fn_.lines[user]};
                if (line.op == "load" && line.operands.size() == 1) {
                    read = true;
                    continue;
                }

                if (line.op != "get_element_ptr" || line.operands.size() != 1) {
                    ok = false;
                    break;
                }

                // The field's address may only be stored through, once
                const auto& stores{users_[user]};
                if (stores.size() != 1 || fn_.lines[stores.front()].op != "store" ||
                    fn_.lines[stores.front()].operands.back() != user ||
                    !address_operand(stores.front()) || !fields.insert(field_key(user)).second) {
                    ok = false;
                    break;
                }
            }
            if (ok && read) { built_.insert(i); }
        }

        // A slot written once, at entry or ahead of every read in one segment, holds that value,
        // so the store is no effect
        for (usize i{0}; i < fn_.lines.size(); ++i) {
            if (fn_.lines[i].op != "alloca" || scratch_.contains(i) || built_.contains(i)) {
                continue;
            }

            std::vector<usize> stores;
            std::vector<usize> loads;
            bool               ok{true};
            for (const auto user : users_[i]) {
                const auto& line{fn_.lines[user]};
                if (line.op == "load" && line.operands.size() == 1) {
                    loads.emplace_back(user);
                    continue;
                }

                const bool target{line.op == "store" && address_operand(user) &&
                                  line.operands.back() == i &&
                                  std::ranges::count(line.operands, i) == 1};
                if (!target) {
                    ok = false;
                    break;
                }
                stores.emplace_back(user);
            }
            if (!ok || stores.size() != 1) { continue; }

            const auto  store{stores.front()};
            const auto& segment{fn_.lines[store].segment};
            const bool  local{std::ranges::all_of(loads, [&](usize load) {
                return load > store && fn_.lines[load].segment == segment;
            })};
            if (segment == 0 || local) { held_.insert(i); }
        }
    }

    // A field address named apart from the slot it is in
    [[nodiscard]] auto field_key(usize gep) const -> std::string {
        std::string key{fn_.lines[gep].text};
        const auto  at{key.find('%')};
        if (at == std::string::npos) { return key; }
        auto end{at + 1};
        while (end < key.size() && key[end] >= '0' && key[end] <= '9') { ++end; }
        return key.replace(at, end - at, "_");
    }

    // A read of a built slot, as the values its fields were given
    [[nodiscard]] auto built_value(usize slot) -> std::string {
        std::vector<std::string> fields;
        for (const auto user : users_[slot]) {
            if (fn_.lines[user].op != "get_element_ptr") { continue; }
            const auto  store{users_[user].front()};
            const auto& text{fn_.lines[store].text};
            const auto  value{text.substr(6, text.rfind(", ") - 6)};
            std::string held{value};
            if (const auto found{fn_.defined.find(value)}; found != fn_.defined.end()) {
                held = bare_.contains(found->second)
                           ? canonical(found->second)
                           : fmt::format("{{#{}}}", final_id(found->second));
            }
            fields.emplace_back(fmt::format("{}={}", field_key(user), held));
        }
        std::ranges::sort(fields);
        std::string out{fmt::format("built {} {{", fn_.lines[slot].text)};
        for (const auto& field : fields) { out += field + ";"; }
        return out + "}";
    }

    [[nodiscard]] auto canonical(usize i) -> const std::string& {
        if (final_[i]) { return *final_[i]; }
        const auto& line{fn_.lines[i]};
        std::string out;
        if (line.op == "load" && !line.operands.empty()) {
            const auto address{raw_tree(line.operands.front())};
            if (built_.contains(line.operands.front())) {
                out = built_value(line.operands.front());
            } else if (auto held{spilled_value(line.operands.front())}) {
                out = std::move(*held);
                bare_.insert(i);
            } else if (auto value{forwarded(address)}) {
                out = std::move(*value);
                bare_.insert(i);
            } else {
                out = fmt::format("load {{#{}}}", final_id(line.operands.front()));
                // A load of memory written after its initializer keeps its place among the effects
                if (!written_once(line.operands.front(), address)) {
                    out += fmt::format("@{}:{}", line.segment, before_[i]);
                }
            }
        } else if (line.op == "alloca" ||
                   (line.op == "load" && by_value_param(line.text.substr(5)))) {
            out = raw_tree(i);
        } else {
            out = expand(line.text, false);
        }
        final_[i] = std::move(out);
        return *final_[i];
    }

    // Whether the memory a load reads is written at most once, at entry. A stack slot counts
    // every store into any part of it
    [[nodiscard]] auto written_once(usize address_line, const std::string& address) const -> bool {
        const std::vector<usize>* writes{nullptr};
        if (const auto root{alloca_root(address_line)}) {
            const auto found{root_writes_.find(*root)};
            if (found == root_writes_.end()) { return true; }
            writes = &found->second;
        } else {
            const auto found{stores_.find(address)};
            if (found == stores_.end()) { return true; }
            if (found->second.size() != 1) { return false; }
            return fn_.lines[found->second.front().line].segment == 0;
        }

        return writes->size() == 1 && fn_.lines[writes->front()].segment == 0;
    }

    function_text                                                      fn_;
    interner&                                                          shared_;
    interner                                                           raw_names_;
    std::vector<u32>                                                   alloca_ids_;
    std::vector<std::vector<usize>>                                    users_;
    std::vector<usize>                                                 before_;
    std::vector<stdx::option<std::string>>                             raw_;
    std::vector<stdx::option<std::string>>                             final_;
    ankerl::unordered_dense::map<std::string, std::vector<store_site>> stores_;
    ankerl::unordered_dense::map<usize, std::vector<usize>>            root_writes_;
    ankerl::unordered_dense::set<usize>                                scratch_;
    ankerl::unordered_dense::set<usize>                                bare_;
    ankerl::unordered_dense::set<usize>                                built_;
    ankerl::unordered_dense::set<usize>                                held_;
};

} // namespace

namespace {

[[nodiscard]] auto one_same(std::string_view lhs, std::string_view rhs) -> bool {
    interner      shared;
    canonicalizer left{parse(lhs), shared};
    canonicalizer right{parse(rhs), shared};
    if (left.segments() != right.segments()) { return false; }
    return left.effects() == right.effects();
}

// The functions a dump holds, each from its header up to the next
[[nodiscard]] auto functions_in(std::string_view text) -> std::vector<std::string_view> {
    std::vector<std::string_view> out;
    for (usize at{0}; at < text.size();) {
        const auto next{text.find("\nfn ", at + 1)};
        const auto end{next == std::string_view::npos ? text.size() : next + 1};
        out.emplace_back(text.substr(at, end - at));
        at = end;
    }
    return out;
}

} // namespace

auto same_effects(std::string_view lhs, std::string_view rhs) -> bool {
    // Functions sharing one name are matched up one to one
    const auto left{functions_in(lhs)};
    const auto right{functions_in(rhs)};
    if (left.size() != right.size()) { return false; }
    std::vector<bool> used(right.size(), false);
    for (const auto function : left) {
        bool matched{false};
        for (usize j{0}; j < right.size() && !matched; ++j) {
            if (used[j] || !one_same(function, right[j])) { continue; }
            used[j] = true;
            matched = true;
        }
        if (!matched) { return false; }
    }
    return true;
}

} // namespace ghoti::tests::helpers
