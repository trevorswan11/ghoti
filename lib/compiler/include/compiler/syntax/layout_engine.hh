#pragma once

#include <ostream>

#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/syntax/doc.hh"

namespace ghoti::syntax {

enum class layout_mode : u8 {
    FLAT,
    BREAK,
};

struct layout_command {
    doc_id      doc;
    u16         indent_cols;
    layout_mode mode;
};

class layout_engine {
  public:
    explicit layout_engine(doc_manager& doc_manager,
                           u16          max_width     = 100,
                           u16          indent_spaces = 4) noexcept
        : doc_manager_{doc_manager}, max_width_{max_width}, indent_spaces_{indent_spaces} {}
    ~layout_engine() = default;
    MAKE_PINNED(layout_engine);

    auto render(std::ostream& os) -> void;
    auto render(doc_id root, std::ostream& os) -> void;

  private:
    // Whether `doc` fits flat in what's left of the line, along with whatever follows it on that
    // line in `rest` (the commands still to render, innermost last)
    auto fits(u32 current_width, doc_id doc, gsl::span<const layout_command> rest) const noexcept
        -> bool;
    auto measure(doc_id doc, i64& width_left) const noexcept -> bool;
    // Measures `doc` as rendered in `mode` until its first line break: true at a break, false once
    // the line overflows, none when it ends first
    auto measure_until_break(doc_id doc, layout_mode mode, i64& width_left) const noexcept
        -> stdx::option<bool>;

  private:
    doc_manager& doc_manager_;
    u16          max_width_;
    u16          indent_spaces_;
};

} // namespace ghoti::syntax
