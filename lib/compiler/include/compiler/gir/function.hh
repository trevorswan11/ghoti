#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/arena.hh>
#include <stdx/assert.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
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
             gir::linkage       linkage      = linkage::INTERNAL,
             std::string        abi_name     = "c") noexcept
        : arena_{arena}, name_{std::move(name)}, type_{type}, is_test_{is_test},
          is_constexpr_{is_constexpr}, is_variadic_{is_variadic}, linkage_{linkage},
          abi_name_{std::move(abi_name)} {}
    ~function() = default;
    MAKE_PINNED(function);

    MAKE_GETTER(name, std::string_view);
    MAKE_GETTER(type, sema::type&);
    MAKE_GETTER(is_test, bool);
    MAKE_GETTER(is_constexpr, bool);
    MAKE_GETTER(is_variadic, bool);
    MAKE_GETTER(linkage, gir::linkage);
    MAKE_GETTER(abi_name, std::string_view);
    MAKE_GETTER(link_name, std::string_view);
    MAKE_GETTER(is_weak, bool);
    MAKE_GETTER(is_naked, bool);
    MAKE_GETTER(calling_conv, ast::calling_convention);
    MAKE_GETTER(test_desc, std::string_view);
    MAKE_GETTER(test_file, std::string_view);
    MAKE_GETTER(test_line, u32);
    MAKE_GETTER(test_column, u32);
    MAKE_DEDUCING_GETTER(params);
    MAKE_DEDUCING_GETTER(segments);

    auto set_link_name(std::string name) -> void { link_name_ = std::move(name); }
    auto set_weak(bool weak) -> void { is_weak_ = weak; }
    auto set_naked(bool naked) -> void { is_naked_ = naked; }
    auto set_calling_conv(ast::calling_convention conv) -> void { calling_conv_ = conv; }
    auto set_test_desc(std::string desc) -> void { test_desc_ = std::move(desc); }
    auto set_test_location(std::string file, u32 line, u32 col) -> void {
        test_file_   = std::move(file);
        test_line_   = line;
        test_column_ = col;
    }

    auto add_param(std::string name, sema::type& param_type) -> parameter&;
    auto add_segment() -> segment&;

    [[nodiscard]] auto get_segment(this auto&& self, segment_id id) -> auto& {
        const auto idx{std::to_underlying(id)};
        ASSERT(idx < self.segments_.size(), "Segment index out of bounds");
        return self.segments_[idx];
    }

    template <typename Self>
    [[nodiscard]] auto get_segment_opt(this Self&& self, segment_id id) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, segment>*> {
        const auto idx{std::to_underlying(id)};
        if (idx >= self.segments_.size()) { return stdx::none; }
        return self.segments_[idx];
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
    bool                    is_weak_{false};
    bool                    is_naked_{false};
    ast::calling_convention calling_conv_{ast::calling_convention::C};
    gir::linkage            linkage_{linkage::INTERNAL};
    std::string             abi_name_{"c"};
    std::string             link_name_;
    std::string             test_desc_;
    std::string             test_file_;
    u32                     test_line_{1};
    u32                     test_column_{1};
};

} // namespace ghoti::gir
