#include <stdx/result.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/ip/index.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

auto analyzer::require_runtime(const source& src) -> result<void> {
    if (is_comptime()) { return comp_.needs_runtime(u_, src); }
    return {};
}

auto analyzer::constant(ip::index value, gs::type& type) -> gir::value {
    return gir::value{value, type};
}

auto analyzer::terminated() const -> bool {
    const auto seg{builder_.get_segment()};
    return seg && seg->has_terminator();
}

auto analyzer::switch_to(gir::segment_id seg) -> void {
    builder_.set_segment(*gir_fn_->get_segment(seg));
}

auto analyzer::new_segment() -> gir::segment_id { return gir_fn_->add_segment().get_id(); }

auto analyzer::emit_goto(gir::segment_id seg) -> void { builder_.emit_goto(seg); }

auto analyzer::set_location(const source& src) -> void {
    if (gir_fn_) { builder_.set_location(comp_.location_of(u_, src)); }
}

} // namespace ghoti::uir::sema
