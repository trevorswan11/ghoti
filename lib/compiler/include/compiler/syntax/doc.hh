#pragma once

#include <string_view>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/iterator.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

namespace ghoti::syntax {

enum class doc_id : u32 {};

// Wadler Doc IR
// https://homepages.inf.ed.ac.uk/wadler/papers/prettier/prettier.pdf
// https://lindig.github.io/papers/strictly-pretty-2000.pdf
namespace docs {

struct text {
    std::string_view text;
};

struct concat {
    std::vector<doc_id> children;
};

struct indent {
    doc_id child;
};

struct group {
    doc_id child;
    bool   force_break{false};
};

struct line_or_space {
    std::string_view space_text{""};
};

struct hard_line {};
struct soft_line {};

struct if_break {
    doc_id when_broken;
    doc_id when_flat;
};

struct align {
    doc_id child;
    u16    columns;
};

} // namespace docs

using doc_t = stdx::variant<docs::text,
                            docs::concat,
                            docs::indent,
                            docs::group,
                            docs::line_or_space,
                            docs::hard_line,
                            docs::soft_line,
                            docs::if_break,
                            docs::align>;

class doc_manager {
  public:
    MAKE_ITERATOR(roots_t, std::vector<doc_id>, roots_);

  public:
    doc_manager()  = default;
    ~doc_manager() = default;
    MAKE_MOVE_ONLY(doc_manager);

    constexpr auto add_root(doc_id id) -> void { roots_.emplace_back(id); }

    // Size only returns the number of roots
    [[nodiscard]] constexpr auto total_doc_count() const noexcept -> usize { return docs_.size(); }

    // Constructs a new doc in-place and returns its ID
    template <typename Data, typename... Args>
    [[nodiscard]] constexpr auto add(Args&&... args) -> doc_id {
        docs_.emplace_back(doc_t{Data{std::forward<Args>(args)...}});
        return doc_id{static_cast<u32>(docs_.size() - 1)};
    }

    [[nodiscard]] constexpr auto operator[](doc_id id) const noexcept -> const doc_t& {
        const auto id_idx{static_cast<u32>(id)};
        ASSERT(id_idx < docs_.size(), "Attempt to access invalid id");
        return docs_[id_idx];
    }

  private:
    roots_t            roots_;
    std::vector<doc_t> docs_;
};

} // namespace ghoti::syntax
