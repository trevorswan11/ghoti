#pragma once

#include <filesystem>
#include <ostream>
#include <string>

#include <gsl/pointers>
#include <stdx/function.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"
#include "support/source_file.hh"

namespace ghoti {

// A file a diagnostic points into, with the path as it should be printed
struct render_source {
    std::filesystem::path             path;
    std::string                       display_path;
    gsl::not_null<const source_file*> file;
};

// Finds the source of a file a label or note points into, when it isn't the primary one
using source_resolver = stdx::function<stdx::option<render_source>(const std::filesystem::path&)>;

struct render_options {
    bool color{false};
    // Longer source lines are trimmed around their spans with ellipses
    usize max_line_width{120};
};

// Renders `diag` rustc-style:
// - a `level[CODE]: message` header
// - a `-->` location, and source snippets with the primary span drawn with `^`
// - secondary labels with `-`, then the notes
// Without a source the header is followed by the location alone. Ends with a newline.
auto render_diagnostic(std::ostream&                         os,
                       const detail::formattable_diagnostic& diag,
                       stdx::option<render_source>           primary,
                       const source_resolver&                resolve,
                       render_options                        options) -> std::ostream&;

} // namespace ghoti
