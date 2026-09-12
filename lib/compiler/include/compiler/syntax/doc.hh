#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <stdx/arena.hh>
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
    doc_manager() : nil_{text("")} {}
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

    // Guaranteed to be a stable document id
    [[nodiscard]] auto nil() const noexcept -> syntax::doc_id { return nil_; }
    [[nodiscard]] auto text(std::string_view s) -> syntax::doc_id;
    [[nodiscard]] auto owned(const std::string& s) -> syntax::doc_id;
    [[nodiscard]] auto concat(std::vector<syntax::doc_id> parts) -> syntax::doc_id;
    [[nodiscard]] auto group(syntax::doc_id child, bool force_break = false) -> syntax::doc_id;
    [[nodiscard]] auto nest(syntax::doc_id child) -> syntax::doc_id;

    // space when flat, newline when broken
    [[nodiscard]] auto line() -> syntax::doc_id;

    // nothing when flat, newline when broken
    [[nodiscard]] auto soft_line() -> syntax::doc_id;
    [[nodiscard]] auto hard_line() -> syntax::doc_id;
    [[nodiscard]] auto if_break(syntax::doc_id when_broken, syntax::doc_id when_flat)
        -> syntax::doc_id;

    // Interleaves `sep` between `items`.
    [[nodiscard]] auto join(std::vector<syntax::doc_id> items, syntax::doc_id sep)
        -> syntax::doc_id;

    // `open pad items pad close` as a group: one line if it fits, unless `force_break`.
    [[nodiscard]] auto delimited(std::string_view            open,
                                 std::string_view            close,
                                 std::vector<syntax::doc_id> items,
                                 bool                        pad,
                                 bool                        trailing_comma,
                                 bool                        force_break = false) -> syntax::doc_id;

    // As above, but `item_trailers[i]` (a trailing line comment, or `nil()`) is placed right after
    // item `i`'s separator comma. Any non-nil trailer forces the list to break.
    [[nodiscard]] auto delimited(std::string_view            open,
                                 std::string_view            close,
                                 std::vector<syntax::doc_id> items,
                                 std::vector<syntax::doc_id> item_trailers,
                                 bool                        pad,
                                 bool                        trailing_comma,
                                 bool                        force_break = false) -> syntax::doc_id;

  private:
    roots_t            roots_;
    std::vector<doc_t> docs_;
    stdx::arena<>      owned_;
    doc_id             nil_;
};

} // namespace ghoti::syntax
