#pragma once

#include <string>
#include <vector>

#include <stdx/assert.hh>
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
    function(std::string name,
             sema::type& type,
             bool        is_test      = false,
             bool        is_constexpr = false) noexcept
        : name_{std::move(name)}, type_{type}, is_test_{is_test}, is_constexpr_{is_constexpr} {}
    ~function() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(function);

    MAKE_GETTER(name, const std::string&);
    MAKE_GETTER(type, sema::type&);
    MAKE_GETTER(is_test, bool);
    MAKE_GETTER(is_constexpr, bool);
    MAKE_DEDUCING_GETTER(params);
    MAKE_DEDUCING_GETTER(segments);

    auto add_param(std::string name, sema::type& param_type) -> parameter&;
    auto add_segment() -> segment&;

    [[nodiscard]] auto get_segment(this auto&& self, usize id) -> auto& {
        ASSERT(id < self.segments_.size(), "Segment index out of bounds");
        return self.segments_[id];
    }

    [[nodiscard]] auto get_segment_opt(this auto&& self, usize id) noexcept {
        if (id >= self.segments_.size()) { return stdx::none; }
        return stdx::option<decltype(self.segments_[id])>{self.segments_[id]};
    }

    auto next_local_id(local_kind kind = local_kind::TEMPORARY) noexcept -> local_id {
        return local_id{next_local_index_++, kind};
    }

    [[nodiscard]] auto local_count() const noexcept -> usize { return next_local_index_; }

  private:
    std::string            name_;
    sema::type&            type_;
    std::vector<parameter> params_;
    std::vector<segment>   segments_;
    bool                   is_test_{false};
    bool                   is_constexpr_{false};
    usize                  next_local_index_{0};
};

} // namespace ghoti::gir
