#pragma once

#include <ostream>

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
    u16         indent_level;
    layout_mode mode;
};

class layout_engine {
  public:
    explicit layout_engine(doc_manager& manager,
                           u16          max_width     = 100,
                           u16          indent_spaces = 4) noexcept
        : manager_{manager}, max_width_{max_width}, indent_spaces_{indent_spaces} {}
    ~layout_engine() = default;
    MAKE_PINNED(layout_engine);

    auto render(doc_id root, std::ostream& os) -> void;

  private:
    auto fits(u32 current_width, doc_id doc) const noexcept -> bool;
    auto measure(doc_id doc, i64& width_left) const noexcept -> bool;

  private:
    doc_manager& manager_;
    u16          max_width_;
    u16          indent_spaces_;
};

} // namespace ghoti::syntax
