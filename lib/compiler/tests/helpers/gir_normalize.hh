#pragma once

#include <string>
#include <string_view>

namespace ghoti::tests::helpers {

// A GIR dump in a form two pipelines can be compared in (§11.1 of the UIR plan):
//   - integer and bool instructions whose operands are all constants are folded, and a
//     `cond_goto` on a constant becomes a `goto`
//   - a segment that only jumps on is threaded through, and segments are numbered in CFG order
//     from the entry, dropping unreachable ones
//   - every `alloca` moves to the top of the entry segment, keeping their order
//   - temporaries are renumbered by first appearance
//   - types, globals and functions are sorted by name
[[nodiscard]] auto normalize_gir(std::string_view dump) -> std::string;

} // namespace ghoti::tests::helpers
