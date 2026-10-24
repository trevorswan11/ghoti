#pragma once

#include <tuple>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/types.hh>

#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"

namespace ghoti::mod { struct module_t; } // namespace ghoti::mod

namespace ghoti::sema {

// The declarations an alias chain (`const A = B;`, `const R = m.R;`) has passed through, so a
// chain that loops back on itself ends instead of spinning
class alias_chain {
  public:
    // Records `owner`'s `node`, false if the chain already passed through it
    [[nodiscard]] auto step(const mod::module_t& owner, ast::node_id node) -> bool {
        const step_key key{&owner, node.get_kind(), node.get_index()};
        return seen_.insert(key).second;
    }

  private:
    using step_key = std::tuple<gsl::not_null<const mod::module_t*>, ast::node_kind, usize>;

  private:
    ankerl::unordered_dense::set<step_key> seen_;
};

} // namespace ghoti::sema
