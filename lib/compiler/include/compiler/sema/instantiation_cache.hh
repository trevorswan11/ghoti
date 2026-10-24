#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/span_ext>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/const_value.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/side_tables.hh"
#include "support/sorted_vector_map.hh"

namespace ghoti::mod { enum class if_branch : u8; } // namespace ghoti::mod

namespace ghoti::sema {

class type;

// Comptime argument values per monomorphization, keyed by mangled name
using comptime_arg_map = ankerl::unordered_dense::map<std::string,
                                                      stdx::box<std::vector<gir::const_value>>,
                                                      stdx::string_transparent_hash,
                                                      stdx::string_transparent_eq>;

// A `fn(...): type` constructor's `comptime` parameters, name -> folded value, so its aggregate's
// member functions can read them at emit time. Keyed by the constructor's mangled name.
using type_ctor_binding_map =
    ankerl::unordered_dense::map<std::string,
                                 stdx::box<std::vector<std::pair<std::string, gir::const_value>>>,
                                 stdx::string_transparent_hash,
                                 stdx::string_transparent_eq>;

// Per-monomorphization body typing, replayed at emit time: `[n]T` with a `comptime n`, and the
// `@This()` shape of a `fn(T): type` constructor's member functions.
struct body_type_diff {
    template <typename T> using table = sorted_vector_map<usize, T>;

    table<stdx::option<type&>>       node_types;
    table<stdx::option<type&>>       explicit_types;
    table<mod::if_branch>            if_branches;
    table<usize>                     match_arms;
    table<stdx::option<std::string>> call_targets;
    table<resolved_attributes>       attributes;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return node_types.empty() && explicit_types.empty() && if_branches.empty() &&
               match_arms.empty() && call_targets.empty() && attributes.empty();
    }

    [[nodiscard]] auto find_attributes(usize idx) const noexcept
        -> stdx::option<const resolved_attributes&> {
        return attributes.find(idx);
    }

    // An outer `none` means the idx doesn't exist, inner means node has no sema type
    [[nodiscard]] auto find_node_type(usize idx) const noexcept
        -> stdx::option<stdx::option<type&>> {
        if (const auto ty{node_types.find(idx)}) { return *ty; }
        return stdx::none;
    }

    // An outer `none` means the idx doesn't exist, inner means the node has no resolved target
    [[nodiscard]] auto find_call_target(usize idx) const noexcept
        -> stdx::option<stdx::option<std::string_view>> {
        const auto target{call_targets.find(idx)};
        if (!target) { return stdx::none; }
        if (*target) { return stdx::option<std::string_view>{**target}; }
        return stdx::option<std::string_view>{stdx::none};
    }

    // An outer `none` means the idx doesn't exist, inner means type has no sema type
    [[nodiscard]] auto find_explicit_type(usize idx) const noexcept
        -> stdx::option<stdx::option<type&>> {
        if (const auto ty{explicit_types.find(idx)}) { return *ty; }
        return stdx::none;
    }

    [[nodiscard]] auto find_if_branch(usize idx) const noexcept -> stdx::option<mod::if_branch> {
        if (const auto branch{if_branches.find(idx)}) { return *branch; }
        return stdx::none;
    }

    [[nodiscard]] auto find_match_arm(usize idx) const noexcept -> stdx::opt_size {
        if (const auto arm{match_arms.find(idx)}) { return *arm; }
        return stdx::none;
    }
};
using body_type_diff_map = ankerl::unordered_dense::map<std::string,
                                                        stdx::box<body_type_diff>,
                                                        stdx::string_transparent_hash,
                                                        stdx::string_transparent_eq>;

struct generic_instantiation_key {
    gsl::not_null<type*>              generic_fn_type;
    type_span                         arg_types;
    gsl::span<const gir::const_value> comptime_args;  // in parameter order
    stdx::option<const type&>         enclosing_type; // a member's receiver instance

    [[nodiscard]] auto hash() const noexcept -> u64 {
        stdx::hasher h{reinterpret_cast<u64>(generic_fn_type.get())};
        h.combine(reinterpret_cast<u64>(enclosing_type.raw()));
        for (const auto arg : arg_types) { h.combine(reinterpret_cast<u64>(arg.get())); }
        for (const auto& cx : comptime_args) { h.combine(cx.hash()); }
        return h.finalize();
    }

    // Comptime arguments match by identity, so `f(-0.0)` and `f(0.0)` are separate instances
    [[nodiscard]] auto operator==(const generic_instantiation_key& other) const noexcept -> bool {
        return generic_fn_type == other.generic_fn_type && enclosing_type == other.enclosing_type &&
               std::ranges::equal(arg_types, other.arg_types) &&
               std::ranges::equal(comptime_args,
                                  other.comptime_args,
                                  [](const gir::const_value& l, const gir::const_value& r) {
                                      return l.identical(r);
                                  });
    }
};

} // namespace ghoti::sema

template <> struct ankerl::unordered_dense::hash<ghoti::sema::generic_instantiation_key> {
    using is_avalanching = void;
    using key_t          = ghoti::sema::generic_instantiation_key;
    [[nodiscard]] auto operator()(const key_t& key) const noexcept { return key.hash(); }
};

namespace ghoti::sema {

class generic_instantiation_cache {
  public:
    generic_instantiation_cache() noexcept = default;
    ~generic_instantiation_cache()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(generic_instantiation_cache);

    [[nodiscard]] auto find(const generic_instantiation_key& key) const noexcept
        -> stdx::option<const generic_instantiation_entry&> {
        if (auto it{cache_.find(key)}; it != cache_.end()) { return it->second; }
        return stdx::none;
    }

    auto insert(generic_instantiation_key key, type& return_type, std::string mangled_name)
        -> void {
        cache_.emplace(std::move(key),
                       generic_instantiation_entry{
                           .return_type  = &return_type,
                           .mangled_name = std::move(mangled_name),
                       });
    }

    // First write wins, so every module sees the instance's first request
    auto add_request(generic_instantiation_request request) -> void {
        auto name{request.mangled_name};
        requests_.try_emplace(std::move(name), std::move(request));
    }

    [[nodiscard]] auto get_request(std::string_view mangled_name) const noexcept
        -> stdx::option<const generic_instantiation_request&> {
        if (const auto it{requests_.find(mangled_name)}; it != requests_.end()) {
            return it->second;
        }
        return stdx::none;
    }

    // Emit-time replay data, keyed by mangled instantiation name
    auto set_body_type_diff(std::string key, body_type_diff diff) -> void {
        if (body_type_diffs_.contains(key)) { return; }
        body_type_diffs_.insert_or_assign(std::move(key),
                                          stdx::box<body_type_diff>::make(std::move(diff)));
    }

    [[nodiscard]] auto get_body_type_diff(std::string_view key) const noexcept
        -> stdx::option<const body_type_diff&> {
        if (const auto it{body_type_diffs_.find(key)}; it != body_type_diffs_.end()) {
            return *it->second;
        }
        return stdx::none;
    }

    // First write wins
    auto set_comptime_args(std::string key, std::vector<gir::const_value> args) -> void {
        if (comptime_args_.contains(key)) { return; }
        comptime_args_.insert_or_assign(
            std::move(key), stdx::make_box<std::vector<gir::const_value>>(std::move(args)));
    }

    [[nodiscard]] auto get_comptime_args(std::string_view key) const noexcept
        -> stdx::option<const std::vector<gir::const_value>&> {
        if (const auto it{comptime_args_.find(key)}; it != comptime_args_.end()) {
            return *it->second;
        }
        return stdx::none;
    }

    // Marks which parameters an argument made implicitly `comptime` in the monomorph `key`
    auto set_implicit_comptime(std::string key, std::vector<bool> mask) -> void {
        implicit_comptime_.try_emplace(std::move(key), std::move(mask));
    }

    // Whether the parameter at `idx` of the monomorph `key` takes its argument at compile time
    [[nodiscard]] auto binds_comptime(std::string_view key, usize idx, bool declared) const
        -> bool {
        if (declared) { return true; }
        const auto it{implicit_comptime_.find(key)};
        return it != implicit_comptime_.end() && idx < it->second.size() && it->second[idx];
    }

    // First write wins
    auto set_type_ctor_bindings(std::string                                           key,
                                std::vector<std::pair<std::string, gir::const_value>> bindings)
        -> void {
        if (type_ctor_bindings_.contains(key)) { return; }
        type_ctor_bindings_.insert_or_assign(
            std::move(key),
            stdx::make_box<std::vector<std::pair<std::string, gir::const_value>>>(
                std::move(bindings)));
    }

    [[nodiscard]] auto get_type_ctor_bindings(std::string_view key) const noexcept
        -> stdx::option<const std::vector<std::pair<std::string, gir::const_value>>&> {
        if (const auto it{type_ctor_bindings_.find(key)}; it != type_ctor_bindings_.end()) {
            return *it->second;
        }
        return stdx::none;
    }

    // The name `definition`'s instances are mangled under. Same-named generics declared apart
    // (two modules' private `helper`s) are numbered so their instances never share a symbol.
    [[nodiscard]] auto instance_name(std::string_view name, uptr definition) -> std::string {
        auto&      owners{instance_owners_[name]};
        const auto it{std::ranges::find(owners, definition)};
        const auto index{static_cast<usize>(it - owners.begin())};
        if (it == owners.end()) { owners.emplace_back(definition); }
        return index == 0 ? std::string{name} : fmt::format("{}_v{}", name, index + 1);
    }

  private:
    ankerl::unordered_dense::map<generic_instantiation_key, generic_instantiation_entry> cache_;
    ankerl::unordered_dense::map<std::string,
                                 generic_instantiation_request,
                                 stdx::string_transparent_hash,
                                 stdx::string_transparent_eq>
        requests_;
    ankerl::unordered_dense::map<std::string,
                                 std::vector<uptr>,
                                 stdx::string_transparent_hash,
                                 stdx::string_transparent_eq>
                          instance_owners_;
    body_type_diff_map    body_type_diffs_;
    comptime_arg_map      comptime_args_;
    type_ctor_binding_map type_ctor_bindings_;
    ankerl::unordered_dense::map<std::string,
                                 std::vector<bool>,
                                 stdx::string_transparent_hash,
                                 stdx::string_transparent_eq>
        implicit_comptime_;
};

// Every `set_sema_type`/`set_sema_type_if` write `mod::module_t` makes while this is installed as
// its `active_write_log`
struct body_write_log {
    std::vector<usize> node_idxs;
    std::vector<usize> explicit_idxs;
    std::vector<usize> attribute_idxs;
    std::vector<usize> call_idxs;
};

// Snapshots a module's typing side tables so a scoped body resolution can be diffed back out as a
// replayable `body_type_diff`
struct body_typing_snapshot {
    explicit body_typing_snapshot(const mod::module_t& m)
        : nodes{m.sema_side_tables.node_types.values},
          types{m.sema_side_tables.explicit_types.values}, ifs{m.if_comptime_results},
          matches{m.match_arm_results}, calls{m.sema_side_tables.generic_call_targets.values},
          attributes{m.node_attributes} {}

    // Folds every still-deferred `[n]T` under the active `comptime` frame, then records each
    // side-table entry the resolution changed into `out`.
    auto diff_into(context&                            ctx,
                   mod::module_t&                      m,
                   body_type_diff&                     out,
                   stdx::option<const body_write_log&> write_log = stdx::none) const -> void;

    // `diff_into` for one unrolled comptime-loop iteration, plus every slot `write_log` saw written
    // even if it kept the previous iteration's type
    auto diff_iteration_into(context&              ctx,
                             mod::module_t&        m,
                             body_type_diff&       out,
                             const body_write_log& write_log) const -> void;

    // Restores `m`'s side tables and branch cache back to the captured state.
    auto restore_to(mod::module_t& m) const -> void;

    std::vector<stdx::option<type&>>                         nodes;
    std::vector<stdx::option<type&>>                         types;
    ankerl::unordered_dense::map<usize, mod::if_branch>      ifs;
    ankerl::unordered_dense::map<usize, usize>               matches;
    std::vector<stdx::option<std::string>>                   calls;
    ankerl::unordered_dense::map<usize, resolved_attributes> attributes;
};

} // namespace ghoti::sema
