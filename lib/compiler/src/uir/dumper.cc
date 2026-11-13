#include "compiler/uir/dumper.hh"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <gsl/span>
#include <magic_enum/magic_enum.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir {

namespace {

constexpr std::string_view INDENT{"    "};

// Tags whose result nothing reads, printed without a `%N =`
[[nodiscard]] auto is_statement(tag t) -> bool {
    switch (t) {
    case tag::store:
    case tag::yield:
    case tag::ret:
    case tag::break_to:
    case tag::repeat:
    case tag::discard:
    case tag::ret_residual: return true;
    default:                return false;
    }
}

[[nodiscard]] auto quote_text(std::string_view text) -> std::string {
    std::string out{"\""};
    for (const char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\0': out += "\\0"; break;
        default:   out += c; break;
        }
    }
    out += '"';
    return out;
}

class dumper {
  public:
    dumper(const module_t& mod, dump_options options) : mod_{mod}, options_{options} {}

    auto run() -> std::string {
        for (u32 i{0}; i < mod_.imports().size(); ++i) {
            const auto& info{mod_.imports()[i]};
            fmt::format_to(std::back_inserter(out_),
                           "import#{} {}{}\n",
                           i,
                           info.is_library ? info.spec : quote_text(info.spec),
                           info.path ? "" : " (not loaded)");
        }
        for (u32 i{0}; i < mod_.decls().size(); ++i) { decl(i); }
        return std::move(out_);
    }

  private:
    auto decl(u32 index) -> void {
        const auto& d{mod_.decl_at(index)};
        fmt::format_to(
            std::back_inserter(out_), "decl#{} {}", index, magic_enum::enum_name(d.kind));
        if (d.name.len > 0) {
            fmt::format_to(std::back_inserter(out_), " {}", mod_.string(d.name));
        }
        for (const auto flag : magic_enum::enum_values<decl_flags>()) {
            if (flag != decl_flags::NONE && d.has(flag)) {
                fmt::format_to(std::back_inserter(out_), " {}", magic_enum::enum_name(flag));
            }
        }
        if (d.import_index != NO_INDEX) {
            fmt::format_to(std::back_inserter(out_), " import#{}", d.import_index);
        }
        if (d.owner != NO_INDEX) { fmt::format_to(std::back_inserter(out_), " in %{}", d.owner); }
        if (d.extern_target.len > 0) {
            fmt::format_to(
                std::back_inserter(out_), " extern {}", quote_text(mod_.string(d.extern_target)));
        }
        if (d.link_name.len > 0) {
            fmt::format_to(
                std::back_inserter(out_), " link {}", quote_text(mod_.string(d.link_name)));
        }
        out_ += '\n';
        named_body("type", mod_.body(d.type_body), 1);
        named_body("attrs", mod_.body(d.attrs_body), 1);
        named_body("value", mod_.body(d.value_body), 1);
    }

    auto indent(usize depth) -> void {
        for (usize i{0}; i < depth; ++i) { out_ += INDENT; }
    }

    auto named_body(std::string_view name, gsl::span<const u32> body, usize depth) -> void {
        if (body.empty()) { return; }
        indent(depth);
        fmt::format_to(std::back_inserter(out_), "{} {{\n", name);
        for (const auto inst : body) { instruction(inst, depth + 1); }
        indent(depth);
        out_ += "}\n";
    }

    [[nodiscard]] auto ref_text(ref r) const -> std::string {
        if (is_none(r)) { return "_"; }
        if (const auto known{as_well_known(r)}) {
            return fmt::format("@{}", well_known_name(*known));
        }
        return fmt::format("%{}", inst_index(r));
    }

    [[nodiscard]] auto location_text(u32 inst) const -> std::string {
        if (!options_.locations_from) { return {}; }
        const auto& tree{*options_.locations_from};
        const auto  at{mod_.src_of(inst).visit(
            [&tree](ast::node_id id) { return tree.location_of(id); },
            [&tree](ast::explicit_type_id id) { return tree.location_of(id); })};
        return fmt::format(" // {}", at);
    }

    auto instruction(u32 inst, usize depth) -> void {
        const auto  t{mod_.tag_of(inst)};
        const auto& d{mod_.data_of(inst)};
        indent(depth);
        if (!is_statement(t)) { fmt::format_to(std::back_inserter(out_), "%{} = ", inst); }
        out_ += tag_name(t);

        // Bodies print after the header line, so they're gathered while the scalars print
        std::vector<std::pair<std::string, gsl::span<const u32>>> bodies;
        switch (payload_of(t)) {
        case payload_kind::NONE: break;
        case payload_kind::INT: fmt::format_to(std::back_inserter(out_), " {}", d.int_value); break;
        case payload_kind::STR:
            fmt::format_to(std::back_inserter(out_), " {}", quote_text(mod_.string(d.str)));
            break;
        case payload_kind::BIN:
            fmt::format_to(
                std::back_inserter(out_), " {}, {}", ref_text(d.bin.lhs), ref_text(d.bin.rhs));
            break;
        case payload_kind::UN: unary(t, d); break;
        case payload_kind::PL: {
            record_reader reader{mod_, d.pl.extra};
            const auto    header_end{out_.size()};
            out_ += " ";
            fields(schema_of(t), reader, bodies, "");
            if (out_.size() == header_end + 1) { out_.pop_back(); }
            break;
        }
        }
        out_ += location_text(inst);
        out_ += '\n';
        for (const auto& [name, body] : bodies) { named_body(name, body, depth + 1); }
    }

    auto unary(tag t, const data& d) -> void {
        switch (t) {
        case tag::decl_val:
        case tag::decl_ref: fmt::format_to(std::back_inserter(out_), " decl#{}", d.un.aux); return;
        case tag::import: fmt::format_to(std::back_inserter(out_), " import#{}", d.un.aux); return;
        case tag::capture:
            fmt::format_to(std::back_inserter(out_), " capture#{}", d.un.aux);
            return;
        case tag::builtin_ref:
            fmt::format_to(std::back_inserter(out_),
                           " {}",
                           syntax::builtin_name(static_cast<syntax::builtin_t>(d.un.aux)));
            return;
        default: fmt::format_to(std::back_inserter(out_), " {}", ref_text(d.un.operand)); return;
        }
    }

    // Prints scalar fields inline, `name=value`, and queues bodies under a dotted path name
    auto fields(gsl::span<const field>                                     schema,
                record_reader&                                             reader,
                std::vector<std::pair<std::string, gsl::span<const u32>>>& bodies,
                std::string_view                                           prefix) -> void {
        bool       first{true};
        const auto separate{[&] {
            if (!first) { out_ += ", "; }
            first = false;
        }};
        for (const auto& f : schema) {
            const auto path{prefix.empty() ? std::string{f.name}
                                           : fmt::format("{}.{}", prefix, f.name)};
            switch (f.kind) {
            case field_kind::REF: {
                const auto r{reader.ref_field()};
                if (is_none(r)) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}={}", f.name, ref_text(r));
                break;
            }
            case field_kind::U32: {
                const auto value{reader.u32_field()};
                if (value == 0 && !f.flag_names.empty()) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}={}", f.name, scalar(f, value));
                break;
            }
            case field_kind::DECL:
                separate();
                fmt::format_to(std::back_inserter(out_), "decl#{}", reader.u32_field());
                break;
            case field_kind::STR: {
                const auto text{reader.str_field()};
                if (text.empty()) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}={}", f.name, quote_text(text));
                break;
            }
            case field_kind::BODY: {
                const auto body{reader.body_field()};
                if (!body.empty()) { bodies.emplace_back(path, body); }
                break;
            }
            case field_kind::REFS: {
                const auto refs{reader.refs_field()};
                if (refs.empty()) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}=(", f.name);
                for (usize i{0}; i < refs.size(); ++i) {
                    if (i > 0) { out_ += ", "; }
                    out_ += ref_text(refs[i]);
                }
                out_ += ')';
                break;
            }
            case field_kind::WORDS: {
                const auto words{reader.words_field()};
                if (std::ranges::all_of(words, [](u32 w) { return w == 0; })) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}=[", f.name);
                for (usize i{0}; i < words.size(); ++i) {
                    if (i > 0) { out_ += ' '; }
                    fmt::format_to(std::back_inserter(out_), "{:#x}", words[i]);
                }
                out_ += ']';
                break;
            }
            case field_kind::LIST: {
                const auto n{reader.count()};
                if (n == 0) { break; }
                separate();
                fmt::format_to(std::back_inserter(out_), "{}=[", f.name);
                for (u32 i{0}; i < n; ++i) {
                    if (i > 0) { out_ += "; "; }
                    fields(f.items, reader, bodies, fmt::format("{}[{}]", path, i));
                }
                out_ += ']';
                break;
            }
            }
        }
    }

    [[nodiscard]] static auto scalar(const field& f, u32 value) -> std::string {
        if (f.describe) { return std::string{f.describe(value)}; }
        if (f.flag_names.empty()) { return fmt::format("{}", value); }
        std::string out;
        for (usize bit{0}; bit < f.flag_names.size(); ++bit) {
            if ((value & (1U << bit)) == 0 || f.flag_names[bit].empty()) { continue; }
            if (!out.empty()) { out += '|'; }
            out += f.flag_names[bit];
        }
        return out;
    }

  private:
    const module_t& mod_;
    dump_options  options_;
    std::string   out_;
};

} // namespace

auto dump(const module_t& mod, dump_options options) -> std::string {
    return dumper{mod, options}.run();
}

} // namespace ghoti::uir
