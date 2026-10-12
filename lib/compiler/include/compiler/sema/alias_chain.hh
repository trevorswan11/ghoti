#pragma once

#include <algorithm>
#include <tuple>
#include <vector>

#include <stdx/types.hh>

#include "compiler/ast/id.hh"

namespace ghoti::mod { struct module; } // namespace ghoti::mod

namespace ghoti::sema {

// The declarations an alias chain (`const A = B;`, `const R = m.R;`) has passed through, so a
// chain that loops back on itself ends instead of spinning
class alias_chain {
  public:
    // Records `owner`'s `node`, false if the chain already passed through it
    [[nodiscard]] auto step(const mod::module& owner, ast::node_id node) -> bool {
        const step_key key{&owner, node.get_kind(), node.get_index()};
        if (std::ranges::contains(seen_, key)) { return false; }
        seen_.push_back(key);
        return true;
    }

  private:
    using step_key = std::tuple<const mod::module*, ast::node_kind, usize>;
    std::vector<step_key> seen_;
};

} // namespace ghoti::sema
