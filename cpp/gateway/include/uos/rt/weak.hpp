#pragma once
// JavaScript `WeakMap` and `WeakSet` over object identity (moved here from uos/rt/http.hpp,
// which still provides them). See docs/rt/kv.md ("Identity") and docs/rt/web.md.
#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace uos::rt {
  // `WeakMap<K, V>` / `WeakSet<K>` keyed by object identity: K is any type with
  // `identity()` (a `const void*`) and `weakIdentity()` (a `std::weak_ptr<const void>`
  // owning that address): rt::Request, rt::Response, rt::Kv, rt::Promise<T>, or a port's own
  // token holder. An entry disappears with its key object, and a later object at the same
  // address is a new key. A dead key's value is released at the next purge (a later `set`).
  // Unlike JS ephemerons, a value that holds its own key keeps it alive.
  template <class K, class V> class WeakMap {
  public:
    // `map.get(key)`: std::nullopt where JS returns undefined.
    std::optional<V> get(const K& key) const {
      const V* value = find(key);
      return value ? std::optional<V>(*value) : std::nullopt;
    }
    // A pointer to the stored value, or nullptr.
    const V* find(const K& key) const {
      auto position = slots_.find(key.identity());
      if (position == slots_.end() || position->second.owner.expired()) return nullptr;
      return &position->second.value;
    }
    V* find(const K& key) {
      auto position = slots_.find(key.identity());
      if (position == slots_.end() || position->second.owner.expired()) return nullptr;
      return &position->second.value;
    }
    bool has(const K& key) const { return find(key) != nullptr; }
    void set(const K& key, V value) {
      if (slots_.size() >= purgeAt_) purge();
      slots_.insert_or_assign(key.identity(), Slot{key.weakIdentity(), std::move(value)});
    }
    bool delete_(const K& key) {
      auto position = slots_.find(key.identity());
      if (position == slots_.end()) return false;
      const bool live = !position->second.owner.expired();
      slots_.erase(position);
      return live;
    }

  private:
    struct Slot {
      std::weak_ptr<const void> owner;
      V value;
    };
    void purge() {
      for (auto position = slots_.begin(); position != slots_.end();) {
        if (position->second.owner.expired()) position = slots_.erase(position);
        else ++position;
      }
      purgeAt_ = slots_.size() * 2 + 64;
    }
    std::unordered_map<const void*, Slot> slots_;
    std::size_t purgeAt_ = 64;
  };

  template <class K> class WeakSet {
  public:
    bool has(const K& key) const { return map_.has(key); }
    void add(const K& key) { map_.set(key, true); }
    bool delete_(const K& key) { return map_.delete_(key); }

  private:
    WeakMap<K, bool> map_;
  };
}
