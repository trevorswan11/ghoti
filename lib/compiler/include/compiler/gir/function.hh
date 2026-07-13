#pragma once

#include <string>
#include <utility>
#include <vector>

#include <stdx/arena.hh>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

struct parameter {
    std::string name;
    sema::type& type;
    local_id    id;
};

class function {
  public:
    function(sema::arena_alloc& arena,
             std::string        name,
             sema::type&        type,
             bool               is_test      = false,
             bool               is_constexpr = false,
             bool               is_variadic  = false,
             gir::linkage       linkage      = linkage::INTERNAL) noexcept
        : arena_{arena}, name_{std::move(name)}, type_{type}, is_test_{is_test},
          is_constexpr_{is_constexpr}, is_variadic_{is_variadic}, linkage_{linkage} {}
    ~function() = default;
    MAKE_PINNED(function);

    MAKE_GETTER(name, const std::string&);
    MAKE_GETTER(type, sema::type&);
    MAKE_GETTER(is_test, bool);
    MAKE_GETTER(is_constexpr, bool);
    MAKE_GETTER(is_variadic, bool);
    MAKE_GETTER(linkage, gir::linkage);
    MAKE_DEDUCING_GETTER(params);
    MAKE_DEDUCING_GETTER(segments);

    auto add_param(std::string name, sema::type& param_type) -> parameter&;
    auto add_segment() -> segment&;

    [[nodiscard]] auto get_segment(this auto&& self, segment_id id) -> auto& {
        const auto idx{std::to_underlying(id)};
        ASSERT(idx < self.segments_.size(), "Segment index out of bounds");
        return self.segments_[idx];
    }

    [[nodiscard]] auto get_segment_opt(this auto&& self, segment_id id) noexcept {
        const auto idx{std::to_underlying(id)};
        if (idx >= self.segments_.size()) { return stdx::none; }
        return stdx::option<decltype(self.segments_[idx])>{self.segments_[idx]};
    }

    auto next_local_id(local_kind kind = local_kind::TEMPORARY) noexcept -> local_id {
        return local_id{next_local_index_++, kind};
    }

    [[nodiscard]] auto local_count() const noexcept -> usize { return next_local_index_; }

  private:
    sema::arena_alloc&      arena_;
    std::string             name_;
    sema::type&             type_;
    std::vector<parameter*> params_;
    std::vector<segment*>   segments_;
    usize                   next_local_index_{0};
    bool                    is_test_{false};
    bool                    is_constexpr_{false};
    bool                    is_variadic_{false};
    gir::linkage            linkage_{linkage::INTERNAL};
};

} // namespace ghoti::gir
