#pragma once
// JavaScript Map and Set: hash containers that iterate in insertion order with the live
// iteration semantics of the language (docs/rt/values.md).
//   - set/add of an existing key keeps its position; delete then set/add moves it to the end;
//   - entries added during iteration are visited, entries deleted before being reached are
//     skipped, clear() during iteration ends it unless entries are added afterwards;
//   - keys compare by SameValueZero: a floating-point NaN key equals NaN, and -0 is stored
//     as +0; other key types use std::hash and operator==; std::string keys can be looked up
//     with a std::string_view;
//   - lookup, insertion and deletion are O(1) on average.
// References to an entry stay valid while any iterator of the container is alive, even if
// the entry is deleted meanwhile (as a JS loop variable would). Iterators must not outlive
// their container.
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::rt {
  namespace detail {
    template <class K>
    K normalizeKey(K key) {
      if constexpr (std::is_floating_point_v<K>) {
        if (key == 0) return K(0);
      }
      return key;
    }

    template <class K>
    struct OrderedHash {
      std::size_t operator()(const K& key) const {
        if constexpr (std::is_floating_point_v<K>) {
          if (std::isnan(key)) return 0x7ff8;
          if (key == 0) return 0;
        }
        return std::hash<K>{}(key);
      }
    };
    template <>
    struct OrderedHash<std::string> {
      using is_transparent = void;
      std::size_t operator()(std::string_view key) const noexcept { return std::hash<std::string_view>{}(key); }
    };

    template <class K>
    struct OrderedEqual {
      bool operator()(const K& a, const K& b) const {
        if constexpr (std::is_floating_point_v<K>) return a == b || (std::isnan(a) && std::isnan(b));
        else return a == b;
      }
    };
    template <>
    struct OrderedEqual<std::string> {
      using is_transparent = void;
      bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    // The entry list of a Map or Set: append-only slots plus a key index. A deleted entry
    // leaves a dead slot; dead slots are reclaimed only while no iterator is alive.
    template <class K, class Entry, class KeyOf>
    class OrderedTable {
    public:
      struct Slot {
        std::unique_ptr<Entry> entry;
        bool alive = false;
      };

      OrderedTable() = default;
      OrderedTable(const OrderedTable& other) { copyFrom(other); }
      OrderedTable(OrderedTable&& other) noexcept = default;
      OrderedTable& operator=(const OrderedTable& other) {
        if (this != &other) {
          OrderedTable copy(other);
          *this = std::move(copy);
        }
        return *this;
      }
      OrderedTable& operator=(OrderedTable&& other) noexcept = default;

      template <class Key>
      Entry* find(const Key& key) const {
        const auto found = index_.find(key);
        return found == index_.end() ? nullptr : slots_[found->second].entry.get();
      }

      // Appends an entry whose key is not present.
      Entry& append(std::unique_ptr<Entry> entry) {
        reclaim();
        const std::size_t position = slots_.size();
        slots_.push_back({std::move(entry), true});
        Entry& stored = *slots_.back().entry;
        index_.emplace(KeyOf{}(stored), position);
        ++live_;
        return stored;
      }

      template <class Key>
      bool erase(const Key& key) {
        const auto found = index_.find(key);
        if (found == index_.end()) return false;
        const std::size_t position = found->second;
        index_.erase(found);
        Slot& slot = slots_[position];
        slot.alive = false;
        if (!iterators_) slot.entry.reset();
        --live_;
        reclaim();
        return true;
      }

      void clear() {
        index_.clear();
        live_ = 0;
        if (!iterators_) {
          slots_.clear();
          return;
        }
        for (auto& slot : slots_) slot.alive = false;
      }

      std::size_t size() const noexcept { return live_; }
      std::size_t slotCount() const noexcept { return slots_.size(); }
      Entry* live(std::size_t position) const noexcept { return slots_[position].alive ? slots_[position].entry.get() : nullptr; }
      void attach() const noexcept { ++iterators_; }
      void detach() const noexcept { --iterators_; }

    private:
      void copyFrom(const OrderedTable& other) {
        for (const auto& slot : other.slots_)
          if (slot.alive) append(std::make_unique<Entry>(*slot.entry));
      }

      // Drops dead slots once they outnumber the live ones, unless iteration is in progress.
      void reclaim() {
        if (iterators_ || slots_.size() - live_ <= live_ + 8) return;
        std::size_t write = 0;
        for (std::size_t read = 0; read < slots_.size(); ++read) {
          if (!slots_[read].alive) continue;
          if (write != read) {
            slots_[write] = std::move(slots_[read]);
            index_.find(KeyOf{}(*slots_[write].entry))->second = write;
          }
          ++write;
        }
        slots_.resize(write);
      }

      std::vector<Slot> slots_;
      std::unordered_map<K, std::size_t, OrderedHash<K>, OrderedEqual<K>> index_;
      std::size_t live_ = 0;
      mutable std::size_t iterators_ = 0;
    };

    // A live iterator over the entries of an OrderedTable, projected to a key, value or entry.
    template <class Table, class Projection>
    class OrderedIterator {
      using Reference = decltype(Projection{}(std::declval<Table&>().live(0)));

    public:
      using iterator_category = std::forward_iterator_tag;
      using difference_type = std::ptrdiff_t;
      using reference = Reference;
      using value_type = typename Projection::value_type;
      using pointer = std::add_pointer_t<std::remove_reference_t<Reference>>;

      OrderedIterator() = default;
      OrderedIterator(const Table* table, std::size_t position) : table_(table), position_(position) {
        if (table_) table_->attach();
        skipDead();
      }
      OrderedIterator(const OrderedIterator& other) : table_(other.table_), position_(other.position_) {
        if (table_) table_->attach();
      }
      OrderedIterator& operator=(const OrderedIterator& other) {
        if (other.table_) other.table_->attach();
        if (table_) table_->detach();
        table_ = other.table_;
        position_ = other.position_;
        return *this;
      }
      ~OrderedIterator() {
        if (table_) table_->detach();
      }

      reference operator*() const { return Projection{}(table_->live(position_)); }
      pointer operator->() const { return std::addressof(**this); }
      OrderedIterator& operator++() {
        ++position_;
        skipDead();
        return *this;
      }
      OrderedIterator operator++(int) {
        OrderedIterator copy = *this;
        ++*this;
        return copy;
      }
      // True once every entry, including those appended so far, has been visited.
      bool atEnd() const noexcept { return !table_ || position_ >= table_->slotCount(); }
      friend bool operator==(const OrderedIterator& a, const OrderedIterator& b) noexcept {
        if (a.atEnd() || b.atEnd()) return a.atEnd() && b.atEnd();
        return a.table_ == b.table_ && a.position_ == b.position_;
      }

    private:
      void skipDead() {
        while (table_ && position_ < table_->slotCount() && !table_->live(position_)) ++position_;
      }

      const Table* table_ = nullptr;
      std::size_t position_ = 0;
    };

    template <class Iterator>
    struct OrderedRange {
      Iterator first;
      Iterator begin() const { return first; }
      Iterator end() const { return Iterator(); }
    };

    template <class K, class V>
    struct MapEntryKey {
      const K& operator()(const std::pair<const K, V>& entry) const noexcept { return entry.first; }
    };
    template <class K, class V>
    struct MapEntryProjection {
      using value_type = std::pair<K, V>;
      std::pair<const K, V>& operator()(std::pair<const K, V>* entry) const noexcept { return *entry; }
    };
    template <class K, class V>
    struct MapKeyProjection {
      using value_type = K;
      const K& operator()(std::pair<const K, V>* entry) const noexcept { return entry->first; }
    };
    template <class K, class V>
    struct MapValueProjection {
      using value_type = V;
      V& operator()(std::pair<const K, V>* entry) const noexcept { return entry->second; }
    };
    template <class K>
    struct SetKey {
      const K& operator()(const K& key) const noexcept { return key; }
    };
    template <class K>
    struct SetProjection {
      using value_type = K;
      const K& operator()(const K* key) const noexcept { return *key; }
    };
  }

  // new Map(): set/get/has/delete_/clear/size, for...of over [key, value] entries.
  template <class K, class V>
  class OrderedMap {
    using Entry = std::pair<const K, V>;
    using Table = detail::OrderedTable<K, Entry, detail::MapEntryKey<K, V>>;

  public:
    using key_type = K;
    using mapped_type = V;
    using value_type = Entry;
    using iterator = detail::OrderedIterator<Table, detail::MapEntryProjection<K, V>>;
    using key_iterator = detail::OrderedIterator<Table, detail::MapKeyProjection<K, V>>;
    using value_iterator = detail::OrderedIterator<Table, detail::MapValueProjection<K, V>>;

    OrderedMap() = default;
    // new Map([[k, v], ...]): a repeated key keeps its first position and its last value.
    OrderedMap(std::initializer_list<std::pair<K, V>> list) {
      for (const auto& [key, value] : list) set(key, value);
    }
    template <class Iterator>
    OrderedMap(Iterator first, Iterator last) {
      for (; first != last; ++first) set(first->first, first->second);
    }

    std::size_t size() const noexcept { return table_.size(); }
    // map.get(key): nullptr when absent (undefined).
    template <class Key>
    V* get(const Key& key) noexcept {
      Entry* entry = table_.find(key);
      return entry ? &entry->second : nullptr;
    }
    template <class Key>
    const V* get(const Key& key) const noexcept {
      const Entry* entry = table_.find(key);
      return entry ? &entry->second : nullptr;
    }
    template <class Key>
    bool has(const Key& key) const noexcept {
      return table_.find(key) != nullptr;
    }
    // map.set(key, value): replaces the value in place, or appends a new entry.
    OrderedMap& set(K key, V value) {
      key = detail::normalizeKey(std::move(key));
      if (Entry* entry = table_.find(key)) entry->second = std::move(value);
      else table_.append(std::make_unique<Entry>(std::move(key), std::move(value)));
      return *this;
    }
    // map.delete(key): true when an entry was removed.
    template <class Key>
    bool delete_(const Key& key) {
      return table_.erase(key);
    }
    void clear() { table_.clear(); }

    iterator begin() const { return iterator(&table_, 0); }
    iterator end() const { return iterator(); }
    detail::OrderedRange<iterator> entries() const { return {begin()}; }
    detail::OrderedRange<key_iterator> keys() const { return {key_iterator(&table_, 0)}; }
    detail::OrderedRange<value_iterator> values() const { return {value_iterator(&table_, 0)}; }
    // map.forEach((value, key) => ...), with the same live semantics as for...of.
    template <class Callback>
    void forEach(Callback&& callback) const {
      for (auto& [key, value] : *this) callback(value, key);
    }

  private:
    Table table_;
  };

  // new Set(): add/has/delete_/clear/size, for...of over the values.
  template <class K>
  class OrderedSet {
    using Table = detail::OrderedTable<K, K, detail::SetKey<K>>;

  public:
    using key_type = K;
    using value_type = K;
    using iterator = detail::OrderedIterator<Table, detail::SetProjection<K>>;

    OrderedSet() = default;
    // new Set([a, b, ...]): duplicates keep their first position.
    OrderedSet(std::initializer_list<K> list) {
      for (const auto& value : list) add(value);
    }
    template <class Iterator>
    OrderedSet(Iterator first, Iterator last) {
      for (; first != last; ++first) add(*first);
    }

    std::size_t size() const noexcept { return table_.size(); }
    template <class Key>
    bool has(const Key& key) const noexcept {
      return table_.find(key) != nullptr;
    }
    OrderedSet& add(K key) {
      key = detail::normalizeKey(std::move(key));
      if (!table_.find(key)) table_.append(std::make_unique<K>(std::move(key)));
      return *this;
    }
    template <class Key>
    bool delete_(const Key& key) {
      return table_.erase(key);
    }
    void clear() { table_.clear(); }

    iterator begin() const { return iterator(&table_, 0); }
    iterator end() const { return iterator(); }
    detail::OrderedRange<iterator> values() const { return {begin()}; }
    detail::OrderedRange<iterator> keys() const { return {begin()}; }
    template <class Callback>
    void forEach(Callback&& callback) const {
      for (const auto& value : *this) callback(value);
    }

  private:
    Table table_;
  };

  // Array.from(iterable) / [...iterable]: a snapshot of a range (Map entries become pairs).
  template <class Range>
  auto arrayFrom(const Range& range) {
    using Iterator = decltype(std::begin(range));
    std::vector<typename std::iterator_traits<Iterator>::value_type> out;
    for (auto it = std::begin(range); it != std::end(range); ++it) out.emplace_back(*it);
    return out;
  }
}
