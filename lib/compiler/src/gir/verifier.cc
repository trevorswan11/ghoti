#include "compiler/gir/verifier.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

namespace {

namespace gs = ghoti::sema;

using gs::signature_of;

[[nodiscard]] auto is_bool_like(const gs::type& t) -> bool {
    if (t.get_kind() == gs::type_kind::BOOL) { return true; }
    const auto vec{t.get_data().as_opt<gs::types::vector>()};
    return vec && vec->underlying.get_kind() == gs::type_kind::BOOL;
}

[[nodiscard]] auto is_comparison(instruction_kind kind) -> bool {
    switch (kind) {
    case instruction_kind::EQ:
    case instruction_kind::NE:
    case instruction_kind::LT:
    case instruction_kind::LE:
    case instruction_kind::GT:
    case instruction_kind::GE: return true;
    default:                   return false;
    }
}

[[nodiscard]] auto is_binary(instruction_kind kind) -> bool {
    switch (kind) {
    case instruction_kind::ADD:
    case instruction_kind::SUB:
    case instruction_kind::MUL:
    case instruction_kind::DIV:
    case instruction_kind::MOD:
    case instruction_kind::AND:
    case instruction_kind::OR:
    case instruction_kind::XOR:
    case instruction_kind::SHL:
    case instruction_kind::SHR: return true;
    default:                    return is_comparison(kind);
    }
}

[[nodiscard]] auto is_unary_or_cast(instruction_kind kind) -> bool {
    switch (kind) {
    case instruction_kind::NEG:
    case instruction_kind::NOT:
    case instruction_kind::BITNOT:
    case instruction_kind::WIDEN_CAST:
    case instruction_kind::INT_CAST:
    case instruction_kind::TRUNC_CAST:
    case instruction_kind::BIT_CAST:
    case instruction_kind::PTR_CAST:
    case instruction_kind::INT_FROM_PTR:
    case instruction_kind::PTR_FROM_INT:
    case instruction_kind::ADDRESS_OF:
    case instruction_kind::DEREF:
    case instruction_kind::LOAD:         return true;
    default:                             return false;
    }
}

// Where a local is defined: its segment and the instruction's position there
struct definition {
    usize segment;
    usize index;
};

class function_verifier {
  public:
    function_verifier(const module_t&                                       mod,
                      const function&                                       fn,
                      const ankerl::unordered_dense::set<std::string_view>& symbols,
                      std::vector<violation>&                               out)
        : mod_{mod}, fn_{fn}, symbols_{symbols}, out_{out} {}

    auto run() -> void {
        if (fn_.get_segments().empty()) { return; }
        reachable_ = fn_.reachable_segments();
        for (const auto& param : fn_.get_params()) { params_.insert(param->id.get_index()); }

        check_control_flow();
        collect_definitions();
        compute_dominators();
        for (usize s{0}; s < fn_.get_segments().size(); ++s) {
            if (!reachable(s)) { continue; }
            const auto& instructions{fn_.get_segments()[s]->get_instructions()};
            for (usize i{0}; i < instructions.size(); ++i) {
                check_instruction(s, i, *instructions[i]);
            }
        }
    }

  private:
    [[nodiscard]] auto reachable(usize s) const -> bool {
        return s < reachable_.size() && reachable_[s];
    }

    auto fail(usize s, usize i, const instruction& inst, std::string message) -> void {
        out_.emplace_back(violation{
            .function = std::string{fn_.get_name()},
            .message  = fmt::format("segment {}, instruction {} ({}): {}",
                                    s,
                                    i,
                                    instruction_kind_name(inst.kind),
                                    message),
        });
    }

    auto fail_function(std::string message) -> void {
        out_.emplace_back(
            violation{.function = std::string{fn_.get_name()}, .message = std::move(message)});
    }

    [[nodiscard]] auto segment_exists(stdx::option<segment_id> id) const -> bool {
        return id && std::to_underlying(*id) < fn_.get_segments().size();
    }

    [[nodiscard]] auto successors(usize s) const -> std::vector<usize> {
        const auto& seg{*fn_.get_segments()[s]};
        const auto  term{seg.get_terminator_opt()};
        if (!term) { return {}; }

        std::vector<usize> next;
        for (const auto id : {term->target_segment, term->true_segment, term->false_segment}) {
            if (segment_exists(id)) { next.emplace_back(std::to_underlying(*id)); }
        }
        return next;
    }

    // Every reachable segment ends in its only terminator, and every branch lands on a segment
    auto check_control_flow() -> void {
        const auto& segments{fn_.get_segments()};
        for (usize s{0}; s < segments.size(); ++s) {
            if (!reachable(s)) { continue; }
            const auto& instructions{segments[s]->get_instructions()};
            if (instructions.empty() || !instructions.back()->is_terminator()) {
                fail_function(fmt::format("segment {} has no terminator", s));
            }

            for (usize i{0}; i + 1 < instructions.size(); ++i) {
                if (instructions[i]->is_terminator()) {
                    fail(s, i, *instructions[i], "a terminator before the end of its segment");
                }
            }
            if (instructions.empty()) { continue; }

            const auto& term{*instructions.back()};
            const auto  last{instructions.size() - 1};
            switch (term.kind) {
            case instruction_kind::GOTO:
                if (!segment_exists(term.target_segment)) {
                    fail(s, last, term, "branches to a segment that doesn't exist");
                }
                break;
            case instruction_kind::COND_GOTO:
                if (!segment_exists(term.true_segment) || !segment_exists(term.false_segment)) {
                    fail(s, last, term, "branches to a segment that doesn't exist");
                }
                break;
            default: break;
            }
        }
    }

    // Each instruction result defines its local once. A store's result names what it writes
    auto collect_definitions() -> void {
        const auto& segments{fn_.get_segments()};
        for (usize s{0}; s < segments.size(); ++s) {
            if (!reachable(s)) { continue; }
            const auto& instructions{segments[s]->get_instructions()};
            for (usize i{0}; i < instructions.size(); ++i) {
                const auto& inst{*instructions[i]};
                if (!inst.result || inst.kind == instruction_kind::STORE) { continue; }
                if (!defs_.emplace(*inst.result, definition{.segment = s, .index = i}).second) {
                    fail(s, i, inst, fmt::format("redefines local %{}", inst.result->get_index()));
                }
            }
        }
    }

    // The immediate dominators of the reachable segments (Cooper, Harvey and Kennedy)
    auto compute_dominators() -> void {
        const auto                           count{fn_.get_segments().size()};
        std::vector<usize>                   order;
        std::vector<bool>                    seen(count, false);
        std::vector<std::pair<usize, usize>> stack{{0, 0}};
        seen[0] = true;
        while (!stack.empty()) {
            auto& [s, next]{stack.back()};
            const auto succ{successors(s)};
            if (next < succ.size()) {
                const auto to{succ[next++]};
                if (!seen[to]) {
                    seen[to] = true;
                    stack.emplace_back(to, 0);
                }
                continue;
            }
            order.emplace_back(s);
            stack.pop_back();
        }

        // `order` is a postorder, so its reverse visits a segment's dominators first
        rpo_index_.assign(count, NONE);
        for (usize k{0}; k < order.size(); ++k) { rpo_index_[order[k]] = order.size() - 1 - k; }
        std::vector<std::vector<usize>> preds(count);
        for (const auto s : order) {
            for (const auto to : successors(s)) { preds[to].emplace_back(s); }
        }

        idom_.assign(count, NONE);
        idom_[0] = 0;
        bool changed{true};
        while (changed) {
            changed = false;
            for (auto it{order.rbegin()}; it != order.rend(); ++it) {
                const auto s{*it};
                if (s == 0) { continue; }
                usize dom{NONE};
                for (const auto p : preds[s]) {
                    if (idom_[p] == NONE) { continue; }
                    dom = dom == NONE ? p : intersect(p, dom);
                }
                if (dom != idom_[s]) {
                    idom_[s] = dom;
                    changed  = true;
                }
            }
        }
    }

    [[nodiscard]] auto intersect(usize a, usize b) const -> usize {
        while (a != b) {
            while (rpo_index_[a] > rpo_index_[b]) { a = idom_[a]; }
            while (rpo_index_[b] > rpo_index_[a]) { b = idom_[b]; }
        }
        return a;
    }

    [[nodiscard]] auto dominates(usize a, usize b) const -> bool {
        if (idom_[b] == NONE) { return false; }
        while (b != a) {
            if (b == 0) { return false; }
            b = idom_[b];
        }
        return true;
    }

    auto check_local_use(usize s, usize i, const instruction& inst, local_id id) -> void {
        if (id.is_param()) {
            if (!params_.contains(id.get_index())) {
                fail(s,
                     i,
                     inst,
                     fmt::format("uses parameter %{}, which the function lacks", id.get_index()));
            }
            return;
        }

        const auto def{defs_.find(id)};
        if (def == defs_.end()) {
            fail(s, i, inst, fmt::format("uses local %{}, which nothing defines", id.get_index()));
            return;
        }

        const auto& at{def->second};
        const bool  before{at.segment == s ? at.index < i : dominates(at.segment, s)};
        if (!before) {
            fail(s,
                 i,
                 inst,
                 fmt::format("uses local %{} where its definition doesn't dominate it",
                             id.get_index()));
        }
    }

    auto check_operands(usize s, usize i, const instruction& inst) -> void {
        const auto pool_size{mod_.constants().size()};
        const auto check{[&](const value& v) {
            if (v.is_local()) {
                check_local_use(s, i, inst, v.local());
            } else if (std::to_underlying(v.constant_index()) >= pool_size) {
                fail(s, i, inst, "a constant operand outside the intern pool");
            }
        }};
        for (const auto& v : inst.operands) { check(v); }
        if (inst.asm_info) {
            for (const auto& v : inst.asm_info->output_addrs) { check(v); }
        }
        if (inst.kind == instruction_kind::STORE && inst.result) {
            check_local_use(s, i, inst, *inst.result);
        }
    }

    auto check_arity(usize s, usize i, const instruction& inst) -> void {
        const auto n{inst.operands.size()};
        const auto expect{[&](bool ok, std::string_view what) {
            if (!ok) { fail(s, i, inst, fmt::format("has {} operands, expected {}", n, what)); }
        }};
        if (is_binary(inst.kind)) {
            expect(n == 2, "2");
        } else if (is_unary_or_cast(inst.kind)) {
            expect(n == 1, "1");
        } else {
            switch (inst.kind) {
            case instruction_kind::ALLOCA:
            case instruction_kind::GLOBAL_ADDR:
            case instruction_kind::GOTO:
            case instruction_kind::UNREACHABLE: expect(n == 0, "none"); break;
            case instruction_kind::STORE:
                expect(n == (inst.result ? 1 : 2), "a value and a destination");
                break;
            case instruction_kind::GET_ELEMENT_PTR: expect(n >= 2, "a base and an index"); break;
            case instruction_kind::COND_GOTO:       expect(n == 1, "1"); break;
            case instruction_kind::RET:             expect(n <= 1, "at most 1"); break;
            default:                                break;
            }
        }

        // Every instruction that defines a local says what type it holds
        if (inst.result && inst.kind != instruction_kind::STORE && !inst.type) {
            fail(s, i, inst, "defines a local without a type");
        }
    }

    auto check_types(usize s, usize i, const instruction& inst) -> void {
        if (inst.kind == instruction_kind::COND_GOTO && !inst.operands.empty()) {
            const auto& cond{inst.operands.front()};
            if (cond.type && cond.type->get_kind() != gs::type_kind::BOOL) {
                fail(s,
                     i,
                     inst,
                     fmt::format("branches on a '{}', not a 'bool'",
                                 gs::type_kind_display_name(*cond.type)));
            }
        }

        if (is_comparison(inst.kind) && inst.type && !is_bool_like(*inst.type)) {
            fail(s,
                 i,
                 inst,
                 fmt::format("compares into a '{}', not a 'bool'",
                             gs::type_kind_display_name(*inst.type)));
        }

        if (inst.kind == instruction_kind::RET) { check_return(s, i, inst); }
        if (inst.kind == instruction_kind::CALL) { check_call(s, i, inst); }
        if ((inst.kind == instruction_kind::CALL || inst.kind == instruction_kind::GLOBAL_ADDR ||
             inst.kind == instruction_kind::MAKE_CALLABLE) &&
            inst.callee_name && !symbols_.contains(*inst.callee_name)) {
            fail(s, i, inst, fmt::format("names '{}', which the module lacks", *inst.callee_name));
        }
    }

    auto check_return(usize s, usize i, const instruction& inst) -> void {
        const auto sig{signature_of(fn_.get_type())};
        if (!sig) { return; }
        const auto& ret{sig->return_type};
        const bool  returns_value{ret.get_kind() != gs::type_kind::VOID_ &&
                                  ret.get_kind() != gs::type_kind::NORETURN};
        if (!returns_value && !inst.operands.empty() && ret.get_kind() == gs::type_kind::VOID_) {
            fail(s, i, inst, "returns a value from a 'void' function");
        } else if (returns_value && inst.operands.empty() && !fn_.get_is_test()) {
            fail(s,
                 i,
                 inst,
                 fmt::format("returns nothing from a function returning '{}'",
                             gs::type_kind_display_name(ret)));
        }
    }

    auto check_call(usize s, usize i, const instruction& inst) -> void {
        if (!inst.callee_name) { return; }
        const auto callee{[&] -> stdx::option<const function&> {
            for (const auto f : mod_.get_functions()) {
                if (f->get_name() == *inst.callee_name) { return *f; }
            }
            return stdx::none;
        }()};
        if (!callee) { return; }
        const auto sig{signature_of(callee->get_type())};
        if (!sig) { return; }

        const auto args{inst.operands.size()};
        const auto params{sig->params.size()};
        if (sig->is_variadic ? args < params : args != params) {
            fail(s,
                 i,
                 inst,
                 fmt::format("passes {} arguments to '{}', which takes {}",
                             args,
                             *inst.callee_name,
                             params));
        }
    }

    auto check_instruction(usize s, usize i, const instruction& inst) -> void {
        check_arity(s, i, inst);
        check_operands(s, i, inst);
        check_types(s, i, inst);
    }

  private:
    static constexpr usize NONE{static_cast<usize>(-1)};

    const module_t&                                       mod_;
    const function&                                       fn_;
    const ankerl::unordered_dense::set<std::string_view>& symbols_;
    std::vector<violation>&                               out_;
    std::vector<bool>                                     reachable_;
    ankerl::unordered_dense::set<usize>                   params_;
    ankerl::unordered_dense::map<local_id, definition>    defs_;
    std::vector<usize>                                    rpo_index_;
    std::vector<usize>                                    idom_;
};

} // namespace

auto verify(const module_t& mod) -> std::vector<violation> {
    PROFILE_FUNCTION();
    std::vector<violation>                         found;
    ankerl::unordered_dense::set<std::string_view> symbols;
    ankerl::unordered_dense::set<std::string_view> defined;
    for (const auto fn : mod.get_functions()) {
        symbols.insert(fn->get_name());
        // Modules may each declare one `extern` symbol, but only one function may define it
        if (fn->get_linkage() != linkage::EXTERN && !defined.insert(fn->get_name()).second) {
            found.emplace_back(violation{.function = std::string{fn->get_name()},
                                         .message  = "a second function with this symbol"});
        }
        for (const auto& alias : fn->get_aliases()) { symbols.insert(alias.name); }
    }
    // Codegen emits the `dyn` vtables the root module records
    for (const auto& vtable : mod.get_ast_module().dyn_vtables) { symbols.insert(vtable.symbol); }
    for (const auto g : mod.get_globals()) {
        if (!symbols.insert(g->name).second) {
            found.emplace_back(violation{.function = g->name,
                                         .message  = "a global sharing another item's symbol"});
        }
    }

    for (const auto fn : mod.get_functions()) { function_verifier{mod, *fn, symbols, found}.run(); }
    return found;
}

auto format_violations(const std::vector<violation>& found) -> std::string {
    std::string text;
    for (const auto& v : found) { text += fmt::format("'{}': {}\n", v.function, v.message); }
    return text;
}

} // namespace ghoti::gir
