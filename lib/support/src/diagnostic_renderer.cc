#include "support/diagnostic_renderer.hh"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/option.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/source_file.hh"
#include "support/style.hh"
#include "support/unicode.hh"

namespace ghoti {

namespace {

constexpr std::string_view TAB_EXPANSION{"    "};
constexpr std::string_view ELLIPSIS{"…"};
constexpr usize            TRIM_CONTEXT{8};
constexpr usize            MAX_MULTILINE_LINES{6};

enum class mark : u8 {
    PLAIN,
    PRIMARY,
    SECONDARY,
    GUTTER,
    NOTE,
};

class painter {
  public:
    painter(bool color, diagnostic_level level) noexcept : color_{color}, level_{level} {}

    [[nodiscard]] auto paint(std::string_view text, mark m) const -> std::string {
        if (!color_ || m == mark::PLAIN) { return std::string{text}; }
        return fmt::format(style_of(m), "{}", text);
    }

    [[nodiscard]] auto bold(std::string_view text) const -> std::string {
        if (!color_) { return std::string{text}; }
        return fmt::format(fmt::text_style{style::BOLD}, "{}", text);
    }

  private:
    [[nodiscard]] auto style_of(mark m) const noexcept -> fmt::text_style {
        switch (m) {
        case mark::PRIMARY:   return detail::level_style(level_);
        case mark::SECONDARY:
        case mark::GUTTER:    return style::CYAN_BOLD;
        case mark::NOTE:      return style::GREEN_BOLD;
        default:              return style::BASE;
        }
    }

  private:
    bool             color_;
    diagnostic_level level_;
};

// One code point of a source line as drawn, with tabs expanded
struct glyph {
    std::string_view bytes;
    usize            byte_offset;
    usize            column;
    usize            width;
};

class rendered_line {
  public:
    explicit rendered_line(std::string_view text) : bytes_{text.size()} {
        for (usize i{0}; i < text.size();) {
            const auto length{
                std::min(code_point_length(stdx::string::substr(text, i)), text.size() - i)};
            const auto bytes{stdx::string::substr(text, i, length)};
            const auto width{bytes == "\t" ? TAB_EXPANSION.size() : display_width(bytes)};
            if (width == 0 && !glyphs_.empty()) {
                // A combining mark draws inside the glyph before it
                auto& previous{glyphs_.back()};
                previous.bytes = stdx::string::substr(
                    text, previous.byte_offset, i + length - previous.byte_offset);
            } else {
                glyphs_.push_back({bytes == "\t" ? TAB_EXPANSION : bytes, i, width_, width});
                width_ += width;
            }
            i += length;
        }
    }

    // A byte column past the end keeps counting one column per byte
    [[nodiscard]] auto display_column(usize byte_column) const noexcept -> usize {
        if (byte_column >= bytes_) { return width_ + (byte_column - bytes_); }
        const auto it{std::ranges::find_if(
            glyphs_, [byte_column](const glyph& g) { return g.byte_offset >= byte_column; })};
        return it == glyphs_.end() ? width_ : it->column;
    }

    // The first column holding something other than whitespace
    [[nodiscard]] auto indent() const noexcept -> usize {
        const auto it{std::ranges::find_if(
            glyphs_, [](const glyph& g) { return g.bytes != TAB_EXPANSION && g.bytes != " "; })};
        return it == glyphs_.end() ? width_ : it->column;
    }

    // The glyphs that fit entirely within the display columns [from, to)
    [[nodiscard]] auto slice(usize from, usize to) const -> std::string {
        std::string out;
        for (const auto& g : glyphs_) {
            if (g.column >= from && g.column + g.width <= to) { out += g.bytes; }
        }
        return out;
    }

    [[nodiscard]] auto width() const noexcept -> usize { return width_; }

  private:
    std::vector<glyph> glyphs_;
    usize              width_{0};
    usize              bytes_;
};

// The display columns of a trimmed line that stay visible
struct line_window {
    usize left{0};
    usize right{0};

    [[nodiscard]] auto trims_left() const noexcept -> bool { return left > 0; }

    [[nodiscard]] auto map(usize column) const noexcept -> usize {
        const auto clamped{std::clamp(column, left, std::max(left, right))};
        return clamped - left + (trims_left() ? 1 : 0);
    }
};

// A row drawn under a source line: a gutter margin, marks and pipes, then an optional label
class annotation_row {
  public:
    auto set_margin(std::string_view margin, mark m) -> void {
        margin_      = margin;
        margin_mark_ = m;
    }

    auto put(usize column, char c, mark m) -> void {
        if (cells_.size() <= column) { cells_.resize(column + 1, {' ', mark::PLAIN}); }
        cells_[column] = {c, m};
    }

    auto put_label(usize column, std::string_view text, mark m) -> void {
        label_column_ = column;
        label_        = text;
        label_mark_   = m;
    }

    [[nodiscard]] auto render(const painter& paint) const -> std::string {
        std::string out;
        out += paint.paint(margin_, margin_mark_);
        for (usize i{0}; i < cells_.size();) {
            const auto  m{cells_[i].second};
            std::string run;
            for (; i < cells_.size() && cells_[i].second == m; ++i) { run += cells_[i].first; }
            out += paint.paint(run, m);
        }
        if (!label_.empty()) {
            if (label_column_ > cells_.size()) { out.append(label_column_ - cells_.size(), ' '); }
            out += paint.paint(label_, label_mark_);
        }
        return out;
    }

  private:
    std::string                        margin_;
    mark                               margin_mark_{mark::PLAIN};
    std::vector<std::pair<char, mark>> cells_;
    usize                              label_column_{0};
    std::string                        label_;
    mark                               label_mark_{mark::PLAIN};
};

struct annotation {
    source_span      span;
    std::string_view label;
    bool             primary;

    [[nodiscard]] auto mark_kind() const noexcept -> mark {
        return primary ? mark::PRIMARY : mark::SECONDARY;
    }
    [[nodiscard]] auto underline() const noexcept -> char { return primary ? '^' : '-'; }
    [[nodiscard]] auto is_multiline() const noexcept -> bool {
        return span.end.line > span.start.line;
    }
};

struct section {
    render_source           source;
    std::vector<annotation> annotations;
};

[[nodiscard]] auto line_length(const source_file& file, usize line) -> usize {
    return file.line_text(line).transform([](std::string_view t) { return t.size(); }).value_or(0);
}

// Clamps a span to something drawable: never empty, never ending at the start of a line
[[nodiscard]] auto normalize(source_span span, const source_file& file) -> source_span {
    if (span.end < span.start) { span.end = span.start; }
    if (span.end.line > span.start.line && span.end.column == 0) {
        span.end = {span.end.line - 1, line_length(file, span.end.line - 1)};
    }
    if (span.end.line == span.start.line && span.end.column <= span.start.column) {
        span.end.column = span.start.column + 1;
    }
    return span;
}

[[nodiscard]] auto try_resolve(const source_resolver& resolve, const std::filesystem::path& path)
    -> stdx::option<render_source> {
    if (!resolve) { return stdx::none; }
    return resolve(path);
}

[[nodiscard]] auto digits(usize n) -> usize { return fmt::formatted_size("{}", n); }

class snippet_renderer {
  public:
    snippet_renderer(std::ostream& os, const painter& paint, usize gutter, render_options options)
        : os_{os}, paint_{paint}, gutter_{gutter}, options_{options} {}

    auto render(section& sec, std::string_view arrow) -> void {
        const auto& file{*sec.source.file};
        for (auto& a : sec.annotations) { a.span = normalize(a.span, file); }
        pick_margin(sec);

        const auto& first{sec.annotations.front().span.start};
        os_ << fmt::format("{}{} {}:{}\n",
                           std::string(gutter_ + 1, ' '),
                           paint_.paint(arrow, mark::GUTTER),
                           sec.source.display_path,
                           first);
        write_bar();

        const auto          lines{lines_to_show(sec)};
        stdx::option<usize> previous;
        for (const auto line : lines) {
            if (previous && line > *previous + 1) {
                os_ << paint_.paint("...", mark::GUTTER) << '\n';
            }
            render_line(sec, line);
            previous = line;
        }
    }

    auto write_bar() -> void {
        os_ << ' ' << std::string(gutter_, ' ') << paint_.paint(" |", mark::GUTTER) << '\n';
    }

  private:
    struct placed {
        usize             start;
        usize             end;
        const annotation* source;
    };

  private:
    // Only one multi-line span gets the gutter margin, the others shrink to their first line
    auto pick_margin(section& sec) -> void {
        margin_ = stdx::none;
        const auto by_priority{
            [](const annotation& a, const annotation& b) { return a.primary && !b.primary; }};
        std::ranges::stable_sort(sec.annotations, by_priority);
        for (auto& a : sec.annotations) {
            if (!a.is_multiline()) { continue; }
            if (!margin_) {
                margin_ = a;
                continue;
            }
            const auto end{std::max(line_length(*sec.source.file, a.span.start.line),
                                    a.span.start.column + 1)};
            a.span.end = {a.span.start.line, end};
        }
        if (margin_) {
            const rendered_line start_line{
                sec.source.file->line_text(margin_->span.start.line).value_or("")};
            slash_start_ =
                start_line.display_column(margin_->span.start.column) <= start_line.indent();
        }
    }

    [[nodiscard]] auto lines_to_show(const section& sec) const -> std::vector<usize> {
        std::set<usize> lines;
        for (const auto& a : sec.annotations) {
            if (!a.is_multiline()) { lines.insert(a.span.start.line); }
        }
        if (margin_) {
            const auto start{margin_->span.start.line};
            const auto end{margin_->span.end.line};
            if (end - start + 1 <= MAX_MULTILINE_LINES) {
                for (auto l{start}; l <= end; ++l) { lines.insert(l); }
            } else {
                for (const auto l : {start, start + 1, start + 2, end - 1, end}) {
                    lines.insert(l);
                }
            }
        }

        // A single hidden line is cheaper to show than to elide
        std::vector<usize> shown{lines.begin(), lines.end()};
        std::vector<usize> filled;
        for (usize i{0}; i < shown.size(); ++i) {
            if (i > 0 && shown[i] == shown[i - 1] + 2) { filled.push_back(shown[i] - 1); }
            filled.push_back(shown[i]);
        }
        const auto line_count{sec.source.file->line_count()};
        std::erase_if(filled, [line_count](usize l) { return l >= line_count; });
        return filled;
    }

    [[nodiscard]] auto inside_margin(usize line) const noexcept -> bool {
        return margin_ && line > margin_->span.start.line && line <= margin_->span.end.line;
    }

    [[nodiscard]] auto source_margin(usize line) const -> std::string_view {
        if (!margin_) { return ""; }
        if (line == margin_->span.start.line) { return slash_start_ ? "/ " : "  "; }
        return inside_margin(line) ? "| " : "  ";
    }

    // The margin under a line, for rows that sit before the end of the multi-line span
    [[nodiscard]] auto row_margin(usize line) const -> std::string_view {
        if (!margin_) { return ""; }
        const auto& span{margin_->span};
        const bool  open{(line > span.start.line && line < span.end.line) ||
                         (line == span.start.line && slash_start_)};
        return open ? "| " : "  ";
    }

    [[nodiscard]] auto window_for(const rendered_line& text, const section& sec, usize line) const
        -> line_window {
        const auto width{text.width()};
        if (width <= options_.max_line_width) { return {0, width}; }

        stdx::option<std::pair<usize, usize>> covered;
        const auto                            cover{[&](usize from, usize to) {
            covered = covered
                          ? std::pair{std::min(covered->first, from), std::max(covered->second, to)}
                          : std::pair{from, to};
        }};
        for (const auto& a : sec.annotations) {
            if (a.is_multiline() || a.span.start.line != line) { continue; }
            cover(text.display_column(a.span.start.column), text.display_column(a.span.end.column));
        }
        if (margin_ && line == margin_->span.start.line) {
            const auto at{text.display_column(margin_->span.start.column)};
            cover(at, at + 1);
        }
        if (margin_ && line == margin_->span.end.line) {
            const auto at{text.display_column(margin_->span.end.column)};
            cover(at > 0 ? at - 1 : 0, std::max<usize>(at, 1));
        }

        const auto [from, to]{covered.value_or(std::pair<usize, usize>{0, 0})};
        auto left{from > TRIM_CONTEXT ? from - TRIM_CONTEXT : 0};
        auto right{std::min(width, left + options_.max_line_width)};
        if (to > right) { right = std::min(width, to + TRIM_CONTEXT); }
        if (right == width && width > options_.max_line_width) {
            left = std::min(left, width - options_.max_line_width);
        }
        return {left, right};
    }

    auto render_line(const section& sec, usize line) -> void {
        const rendered_line text{sec.source.file->line_text(line).value_or("")};
        const auto          window{window_for(text, sec, line)};

        std::string shown;
        if (window.trims_left()) { shown += ELLIPSIS; }
        shown += text.slice(window.left, window.right);
        if (window.right < text.width()) { shown += ELLIPSIS; }

        const auto content{
            fmt::format("{}{}", paint_.paint(source_margin(line), margin_mark()), shown)};
        os_ << paint_.paint(fmt::format(" {:>{}} |", line + 1, gutter_), mark::GUTTER);
        if (!stdx::string::trim_right(shown).empty() || !source_margin(line).empty()) {
            os_ << ' ' << stdx::string::trim_right(content);
        }
        os_ << '\n';

        std::vector<annotation_row> rows;
        if (margin_ && line == margin_->span.end.line) { rows.push_back(end_marker(text, window)); }
        auto single{single_line_rows(sec, line, text, window)};
        rows.insert(rows.end(), single.begin(), single.end());
        if (margin_ && line == margin_->span.start.line && !slash_start_) {
            rows.push_back(start_marker(text, window));
        }
        for (const auto& row : rows) { write_row(row); }
    }

    [[nodiscard]] auto margin_mark() const noexcept -> mark {
        return margin_ ? margin_->mark_kind() : mark::PLAIN;
    }

    [[nodiscard]] auto start_marker(const rendered_line& text, const line_window& window) const
        -> annotation_row {
        annotation_row row;
        row.set_margin(" _", margin_mark());
        const auto at{window.map(text.display_column(margin_->span.start.column))};
        for (usize c{0}; c < at; ++c) { row.put(c, '_', margin_mark()); }
        row.put(at, margin_->underline(), margin_mark());
        return row;
    }

    [[nodiscard]] auto end_marker(const rendered_line& text, const line_window& window) const
        -> annotation_row {
        annotation_row row;
        row.set_margin("|_", margin_mark());
        const auto end{window.map(text.display_column(margin_->span.end.column))};
        const auto at{end > 0 ? end - 1 : 0};
        for (usize c{0}; c < at; ++c) { row.put(c, '_', margin_mark()); }
        row.put(at, margin_->underline(), margin_mark());
        if (!margin_->label.empty()) { row.put_label(at + 2, margin_->label, margin_mark()); }
        return row;
    }

    // Underlines first, then the rightmost label inline and the others hung below on pipes
    [[nodiscard]] auto single_line_rows(const section&       sec,
                                        usize                line,
                                        const rendered_line& text,
                                        const line_window&   window) const
        -> std::vector<annotation_row> {
        std::vector<placed> on_line;
        for (const auto& a : sec.annotations) {
            if (a.is_multiline() || a.span.start.line != line) { continue; }
            const auto start{window.map(text.display_column(a.span.start.column))};
            const auto end{std::max(window.map(text.display_column(a.span.end.column)), start + 1)};
            on_line.push_back({start, end, &a});
        }
        if (on_line.empty()) { return {}; }
        std::ranges::stable_sort(on_line, {}, &placed::start);

        const auto margin{row_margin(line)};
        const auto new_row{[&] {
            annotation_row row;
            row.set_margin(margin, margin_mark());
            return row;
        }};

        annotation_row underline{new_row()};
        // Secondary marks go down first so a primary mark wins where they overlap
        for (const bool primary : {false, true}) {
            for (const auto& p : on_line) {
                if (p.source->primary != primary) { continue; }
                for (auto c{p.start}; c < p.end; ++c) {
                    underline.put(c, p.source->underline(), p.source->mark_kind());
                }
            }
        }

        std::vector<placed> hanging;
        const auto&         last{on_line.back()};
        const auto          rightmost_end{std::ranges::max(on_line, {}, &placed::end).end};
        if (!last.source->label.empty()) {
            underline.put_label(rightmost_end + 1, last.source->label, last.source->mark_kind());
        }
        for (usize i{0}; i + 1 < on_line.size(); ++i) {
            if (!on_line[i].source->label.empty()) { hanging.push_back(on_line[i]); }
        }

        std::vector<annotation_row> rows{underline};
        if (hanging.empty()) { return rows; }

        annotation_row connectors{new_row()};
        for (const auto& h : hanging) { connectors.put(h.start, '|', h.source->mark_kind()); }
        rows.push_back(connectors);
        for (auto i{hanging.size()}; i-- > 0;) {
            annotation_row labeled{new_row()};
            for (usize j{0}; j < i; ++j) {
                labeled.put(hanging[j].start, '|', hanging[j].source->mark_kind());
            }
            labeled.put_label(
                hanging[i].start, hanging[i].source->label, hanging[i].source->mark_kind());
            rows.push_back(labeled);
        }
        return rows;
    }

    auto write_row(const annotation_row& row) -> void {
        const auto rendered{row.render(paint_)};
        os_ << ' ' << std::string(gutter_, ' ') << paint_.paint(" |", mark::GUTTER);
        if (!stdx::string::trim_right(rendered).empty()) {
            os_ << ' ' << stdx::string::trim_right(rendered);
        }
        os_ << '\n';
    }

  private:
    std::ostream&            os_;
    const painter&           paint_;
    usize                    gutter_;
    render_options           options_;
    stdx::option<annotation> margin_;
    bool                     slash_start_{false};
};

// Splits the primary span and the labels into one section per file, primary file first
class section_builder {
  public:
    section_builder(const detail::formattable_diagnostic& diag,
                    const stdx::option<render_source>&    primary,
                    const source_resolver&                resolve)
        : resolve_{resolve} {
        if (primary && diag.span) {
            sections_.push_back({*primary, {{*diag.span, diag.primary_label, true}}});
        }
        for (const auto& label : diag.labels) { add_label(label, primary); }
    }

    [[nodiscard]] auto sections() -> std::vector<section>& { return sections_; }
    [[nodiscard]] auto unresolved() const -> const std::vector<const diagnostic_label*>& {
        return unresolved_;
    }

  private:
    auto add_label(const diagnostic_label& label, const stdx::option<render_source>& primary)
        -> void {
        const annotation added{label.at.span, label.text, false};
        for (auto& sec : sections_) {
            if (sec.source.path == label.at.path) {
                sec.annotations.push_back(added);
                return;
            }
        }
        if (primary && primary->path == label.at.path) {
            sections_.push_back({*primary, {added}});
            return;
        }
        if (auto source{try_resolve(resolve_, label.at.path)}) {
            sections_.push_back({std::move(*source), {added}});
            return;
        }
        unresolved_.push_back(&label);
    }

  private:
    const source_resolver&               resolve_;
    std::vector<section>                 sections_;
    std::vector<const diagnostic_label*> unresolved_;
};

[[nodiscard]] auto max_line_of(const section& sec) -> usize {
    usize max{0};
    for (const auto& a : sec.annotations) {
        max = std::max({max, a.span.start.line, a.span.end.line});
    }
    return max;
}

auto write_note_line(std::ostream& os, const painter& paint, usize gutter, std::string_view text)
    -> void {
    os << ' ' << std::string(gutter, ' ') << paint.bold(" = note") << ": " << text << '\n';
}

} // namespace

auto render_diagnostic(std::ostream&                         os,
                       const detail::formattable_diagnostic& diag,
                       stdx::option<render_source>           primary,
                       const source_resolver&                resolve,
                       render_options                        options) -> std::ostream& {
    const auto    level{diag.level.value_or(diagnostic_level::ERROR)};
    const painter paint{options.color, level};

    os << paint.paint(fmt::format("{}[{}]", detail::level_name(level), diag.error_name),
                      mark::PRIMARY)
       << paint.bold(fmt::format(": {}", diag.message)) << '\n';

    section_builder builder{diag, primary, resolve};
    auto&           sections{builder.sections()};

    // Located notes get their own snippet, so they count toward the gutter width
    std::vector<std::pair<const diagnostic_note*, render_source>> located_notes;
    std::vector<std::string>                                      plain_notes;
    for (const auto* label : builder.unresolved()) {
        plain_notes.push_back(fmt::format(
            "{} ({}:{})", label->text, label->at.path.generic_string(), label->at.span.start));
    }
    for (const auto& note : diag.notes) {
        if (!note.at) {
            plain_notes.push_back(note.text);
            continue;
        }
        auto source{primary && primary->path == note.at->path
                        ? primary
                        : try_resolve(resolve, note.at->path)};
        if (source) {
            located_notes.emplace_back(&note, std::move(*source));
        } else {
            plain_notes.push_back(fmt::format(
                "{} ({}:{})", note.text, note.at->path.generic_string(), note.at->span.start));
        }
    }

    usize max_line{0};
    for (const auto& sec : sections) { max_line = std::max(max_line, max_line_of(sec)); }
    for (const auto& [note, source] : located_notes) {
        max_line = std::max(max_line, note->at->span.end.line);
    }
    const auto gutter{digits(max_line + 1)};

    snippet_renderer snippets{os, paint, gutter, options};
    if (sections.empty() && primary) {
        os << fmt::format("{}{} {}",
                          std::string(gutter + 1, ' '),
                          paint.paint("-->", mark::GUTTER),
                          primary->display_path);
        if (const auto at{diag.location()}) { os << fmt::format(":{}", *at); }
        os << '\n';
    }
    for (usize i{0}; i < sections.size(); ++i) {
        snippets.render(sections[i], i == 0 && diag.span && primary ? "-->" : ":::");
    }
    if (!plain_notes.empty() && !sections.empty()) { snippets.write_bar(); }
    for (const auto& text : plain_notes) { write_note_line(os, paint, gutter, text); }

    for (const auto& [note, source] : located_notes) {
        os << paint.paint("note", mark::NOTE) << paint.bold(fmt::format(": {}", note->text))
           << '\n';
        section sec{source, {{note->at->span, "", false}}};
        snippets.render(sec, "-->");
    }
    return os;
}

} // namespace ghoti
