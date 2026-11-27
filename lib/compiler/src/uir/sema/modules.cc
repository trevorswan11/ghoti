#include <string_view>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

auto compilation::import_target(const unit&      u,
                                import_id        import,
                                std::string_view name,
                                const source&    src) -> result<operand> {
    const auto& info{u.uir->import_at(import)};
    if (!info.path) { return unsupported(u, src, "an import that didn't load"); }
    for (const auto& target : units_) {
        if (target.source->path == *info.path) { return module_ref{&target, name}; }
    }

    return unsupported(u, src, "an import of a module the program didn't lower");
}

auto compilation::prelude_module() -> stdx::option<module_ref> {
    if (!prelude_unit_) { return stdx::none; }
    return module_ref{&units_[*prelude_unit_], "builtin"};
}

auto compilation::module_member(const module_ref& m,
                                std::string_view  name,
                                const unit&       user,
                                const source&     src) -> result<operand> {
    const auto& target{*m.target};
    const auto& uir{*target.uir};
    for (const auto decl : uir.root_namespace().decls) {
        const auto& d{uir.decl_at(decl)};
        if (uir.string(d.name) != name) { continue; }
        if (target.index != user.index && !d.has(decl_flags::PUBLIC)) {
            return report(user,
                          src,
                          gs::error::ILLEGAL_PRIVATE_ACCESS,
                          fmt::format("Symbol '{}' is private to module '{}'", name, m.name));
        }

        // A use from another module is located at the declaration it reaches
        return analyze_decl(target, decl, target.index == user.index ? src : source{*d.node});
    }

    return report(user,
                  src,
                  gs::error::UNDECLARED_IDENTIFIER,
                  fmt::format("Module '{}' has no member named '{}'", m.name, name));
}

auto analyzer::analyze_import(inst_id inst) -> result<operand> {
    const auto  import{import_id{mod().data_of(inst).un.aux}};
    const auto& info{mod().import_at(import)};
    return comp_.import_target(u_, import, info.spec, src_of(inst));
}

auto analyzer::module_member(const module_ref& m, std::string_view name, const source& src)
    -> result<operand> {
    const auto value{TRY(comp_.module_member(m, name, u_, src))};
    if (const auto decl{module_decl(m, name)}) {
        warn_deprecated_decl(*m.target, *decl, src);
        note_member_decl(src, *m.target, *decl);
    }
    return value;
}

auto analyzer::module_decl(const module_ref& m, std::string_view name) const
    -> stdx::option<decl_id> {
    const auto& uir{*m.target->uir};
    for (const auto decl : uir.root_namespace().decls) {
        if (uir.string(uir.decl_at(decl).name) == name) { return decl; }
    }

    return stdx::none;
}

} // namespace ghoti::uir::sema
