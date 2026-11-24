#include "compiler/ip/pool.hh"

#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/base.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ip/index.hh"
#include "compiler/sema/type.hh"
#include "support/big_int.hh"
#include "support/float128.hh"
#include "support/int128.hh"

namespace ghoti::ip {

namespace {

// A value's identity as text formatted such that no two values share a key
class key_writer {
  public:
    explicit key_writer(const value_data& data, stdx::option<sema::type&> type) {
        fmt::format_to(std::back_inserter(key_), "{}|", data.index());
        if (type) {
            fmt::format_to(
                std::back_inserter(key_), "{}", std::to_underlying(type->get_ip_index()));
        }
        key_ += '|';
        data.visit([this](const auto& payload) { write(payload); });
    }

    [[nodiscard]] auto take() noexcept -> std::string { return std::move(key_); }

  private:
    auto text(std::string_view s) -> void {
        fmt::format_to(std::back_inserter(key_), "{}:", s.size());
        key_ += s;
    }
    auto element(stdx::option<index> i) -> void {
        if (i) {
            fmt::format_to(std::back_inserter(key_), "{},", std::to_underlying(*i));
        } else {
            key_ += "_,";
        }
    }
    auto elements(const std::vector<index>& items) -> void {
        fmt::format_to(std::back_inserter(key_), "{}[", items.size());
        for (const auto i : items) { element(i); }
        key_ += ']';
    }

    auto write(i64 v) -> void { fmt::format_to(std::back_inserter(key_), "{}", v); }
    auto write(u64 v) -> void { fmt::format_to(std::back_inserter(key_), "{}", v); }
    auto write(const i128& v) -> void { fmt::format_to(std::back_inserter(key_), "{}", v); }
    auto write(const u128& v) -> void { fmt::format_to(std::back_inserter(key_), "{}", v); }
    auto write(const big_int& v) -> void { key_ += v.to_string(); }
    // By bit pattern, so `-0.0` and `0.0` stay apart and a NaN matches itself
    auto write(const f128& v) -> void { fmt::format_to(std::back_inserter(key_), "{}", v.bits()); }
    auto write(bool v) -> void { key_ += v ? '1' : '0'; }
    auto write(const std::string& v) -> void { text(v); }
    auto write(const aggregate_value& v) -> void {
        fmt::format_to(std::back_inserter(key_), "{}[", v.elements.size());
        for (const auto e : v.elements) { element(e); }
        key_ += ']';
    }
    auto write(const enum_value& v) -> void {
        text(v.name);
        fmt::format_to(std::back_inserter(key_), "{}", v.value);
    }
    auto write(const union_value& v) -> void {
        text(v.active_field);
        elements(v.payload);
    }
    auto write(const address_value& v) -> void {
        text(v.symbol);
        elements(v.pointee);
    }
    auto write(const memory_value& v) -> void {
        fmt::format_to(std::back_inserter(key_),
                       "{}[{}]{}+{}",
                       v.alloc,
                       fmt::join(v.path, ","),
                       v.offset,
                       v.len);
    }
    auto write(const dyn_value& v) -> void {
        text(v.data_symbol);
        text(v.vtable_symbol);
    }
    template <typename Marker> auto write(const Marker&) -> void {}

  private:
    std::string key_;
};

} // namespace

auto pool::type_for(const sema::types::key_t& key) -> gsl::not_null<sema::type*> {
    if (const auto it{type_keys_.find(key)}; it != type_keys_.end()) {
        return &type_at(it->second);
    }
    const gsl::not_null<sema::type*> created{arena_.make<sema::type>(key).get()};
    const auto                       at{next_index()};
    created->ip_index_ = at;
    entries_.emplace_back(*created, 0);
    type_keys_.emplace(key, at);
    return created;
}

auto pool::type_at(index i) const noexcept -> sema::type& {
    const auto& e{entries_[std::to_underlying(i)]};
    ASSERT(e.type_entry, "The intern pool entry is a value, not a type");
    return *e.type_entry;
}

auto pool::is_type(index i) const noexcept -> bool {
    return entries_[std::to_underlying(i)].type_entry.has_value();
}

auto pool::intern(value_data data, stdx::option<sema::type&> type) -> index {
    auto key{key_writer{data, type}.take()};
    if (const auto it{value_keys_.find(key)}; it != value_keys_.end()) { return it->second; }

    const auto at{next_index()};
    entries_.emplace_back(stdx::none, static_cast<u32>(values_.size()));
    values_.emplace_back(std::move(data), type);
    value_keys_.emplace(std::move(key), at);
    return at;
}

auto pool::value_at(index i) const noexcept -> const value& {
    const auto& e{entries_[std::to_underlying(i)]};
    ASSERT(!e.type_entry, "The intern pool entry is a type, not a value");
    return values_[e.value_slot];
}

} // namespace ghoti::ip
