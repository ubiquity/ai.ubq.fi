#include "uos/rt/env.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace uos::rt::env {
  // The view: name/value pairs in insertion order.
  struct ScopedEnvironment::Store {
    std::vector<std::pair<std::string, std::string>> entries;

    std::pair<std::string, std::string>* find(std::string_view name) {
      for (auto& entry : entries)
        if (entry.first == name) return &entry;
      return nullptr;
    }

    void assign(std::string_view name, std::string_view value) {
      if (auto* entry = find(name)) entry->second = std::string(value);
      else entries.emplace_back(std::string(name), std::string(value));
    }
  };

  namespace {
    using Store = ScopedEnvironment::Store;

    // The OS environment, first position kept and a later duplicate's value winning.
    std::unique_ptr<Store> fromEnviron() {
      auto store = std::make_unique<Store>();
#if defined(__APPLE__)
      char** environment = *_NSGetEnviron();
#else
      char** environment = environ;
#endif
      for (char** cursor = environment; cursor && *cursor; ++cursor) {
        const std::string_view entry = *cursor;
        const auto separator = entry.find('=');
        if (separator == std::string_view::npos) continue;
        store->assign(entry.substr(0, separator), entry.substr(separator + 1));
      }
      return store;
    }

    std::unique_ptr<Store>& slot() {
      static std::unique_ptr<Store> store;
      return store;
    }

    Store& current() {
      auto& store = slot();
      if (!store) store = fromEnviron();
      return *store;
    }

    // Named entries share the sort key above the largest canonical array index.
    std::uint32_t enumerationOrder(std::string_view key) {
      constexpr auto named = std::numeric_limits<std::uint32_t>::max();
      if (key.size() > 1 && key.front() == '0') return named;
      std::uint32_t index = 0;
      const auto end = key.data() + key.size();
      const auto [next, error] = std::from_chars(key.data(), end, index);
      return next == end && error == std::errc{} ? index : named;
    }
  }

  std::optional<std::string> get(std::string_view name) {
    if (const auto* entry = current().find(name)) return entry->second;
    return std::nullopt;
  }

  bool has(std::string_view name) { return current().find(name) != nullptr; }

  void set(std::string_view name, std::string_view value) { current().assign(name, value); }

  bool remove(std::string_view name) {
    auto& entries = current().entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == name; }), entries.end());
    return true;
  }

  std::vector<std::pair<std::string, std::string>> entries() {
    auto ordered = current().entries;
    std::ranges::stable_sort(ordered, {}, [](const auto& entry) { return enumerationOrder(entry.first); });
    return ordered;
  }

  std::vector<std::string> keys() {
    std::vector<std::string> out;
    for (auto& entry : entries()) out.push_back(std::move(entry.first));
    return out;
  }

  ScopedEnvironment::ScopedEnvironment(std::vector<std::pair<std::string, std::string>> variables)
    : saved_(std::make_unique<Store>()) {
    for (const auto& [name, value] : variables) saved_->assign(name, value);
    current();
    saved_.swap(slot());
  }

  ScopedEnvironment::~ScopedEnvironment() { saved_.swap(slot()); }

  ScopedOverride::ScopedOverride(std::vector<std::pair<std::string, std::optional<std::string>>> updates) {
    previous_.reserve(updates.size());
    for (const auto& [name, value] : updates) {
      previous_.emplace_back(name, get(name));
      if (value) set(name, *value);
      else remove(name);
    }
  }

  ScopedOverride::~ScopedOverride() {
    for (auto entry = previous_.rbegin(); entry != previous_.rend(); ++entry) {
      if (entry->second) set(entry->first, *entry->second);
      else remove(entry->first);
    }
  }
}
