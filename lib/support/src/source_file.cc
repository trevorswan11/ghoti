#include "support/source_file.hh"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/string.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/unicode.hh"

namespace ghoti {

line_offsets::line_offsets(std::string_view input) {
    PROFILE_FUNCTION();
    offsets_.emplace_back(0);
    for (usize i{0}; i < input.size(); ++i) {
        if (input[i] == '\n') {
            offsets_.emplace_back(i + 1);
        } else if (input[i] == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') {
                offsets_.emplace_back(i + 2);
                ++i;
            } else {
                offsets_.emplace_back(i + 1);
            }
        }
    }
}

auto source_file::get_diagnostic_strings_at(const source_location& loc) const
    -> std::pair<std::string_view, stdx::option<std::string>> {
    PROFILE_FUNCTION();
    if (loc.line >= offsets_.size()) { return {"<invalid line>", stdx::none}; }

    const auto start{offsets_[loc.line]};
    const auto end{loc.line + 1 < offsets_.size() ? offsets_[loc.line + 1] : source_.size()};
    auto       substr{stdx::string::substr(source_, start, end - start)};

    // Count skipped on the left but not right since the caret is right-clipped
    usize skipped{0};
    substr = stdx::string::trim_left(substr, [&skipped](char c) -> bool {
        if (std::isspace(c)) {
            skipped += 1;
            return true;
        }
        return false;
    });
    substr = stdx::string::trim_right(substr);

    // Adjust the column number based on skipped spaces
    if (substr.empty() || loc.column < skipped) { return {substr, stdx::none}; }
    const auto true_col{loc.column - skipped};

    // Allow 1 past the end to accommodate missing semicolons
    if (true_col > substr.size() + 1) { return {substr, stdx::none}; }

    // The caret sits under the column's character, measured in display columns: a combining
    // mark takes none and a wide character or emoji two. Tabs are copied so they line up.
    const auto  prefix{substr.substr(0, std::min(true_col, substr.size()))};
    std::string caret_line;
    usize       run_start{0};
    const auto  pad_run{[&](usize end) {
        caret_line.append(display_width(prefix.substr(run_start, end - run_start)), ' ');
    }};
    for (usize i{0}; i < prefix.size(); ++i) {
        if (prefix[i] != '\t') { continue; }
        pad_run(i);
        caret_line += '\t';
        run_start = i + 1;
    }
    pad_run(prefix.size());
    caret_line.append(true_col - prefix.size(), ' ');
    caret_line += '^';

    return {substr, std::move(caret_line)};
}

} // namespace ghoti
