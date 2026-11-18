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
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <list>
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

    template <class K, class Entry, auto KeyOf = std::identity{}>
    class OrderedTable {
      struct Node {
        template <class... Args>
        explicit Node(Args&&... args) : entry(std::forward<Args>(args)...) {}
        Entry entry;
        bool retired = false;
      };

    public:
      using entry_type = Entry;
      using position_type = typename std::list<Node>::iterator;

      OrderedTable() = default;
      OrderedTable(const OrderedTable& other) {
        for (const auto& node : other.entries_)
          if (!node.retired) append(node.entry);
      }
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
        return found == index_.end() ? nullptr : std::addressof(found->second->entry);
      }

      // Appends an entry whose key is not present.
      template <class... Args>
      void append(Args&&... args) {
        const auto node = entries_.emplace(entries_.end(), std::forward<Args>(args)...);
        try {
          index_.emplace(std::invoke(KeyOf, node->entry), node);
        } catch (...) {
          entries_.erase(node);
          throw;
        }
      }

      template <class Key>
      bool erase(const Key& key) {
        const auto found = index_.find(key);
        if (found == index_.end()) return false;
        const auto node = found->second;
        index_.erase(found);
        if (iterators_) {
          node->retired = true;
          retired_ = true;
        } else entries_.erase(node);
        return true;
      }

      void clear() {
        if (iterators_ && !index_.empty()) {
          for (const auto& item : index_) item.second->retired = true;
          retired_ = true;
        }
        index_.clear();
        if (!iterators_) entries_.clear();
      }

      std::size_t size() const noexcept { return index_.size(); }
      position_type begin() const noexcept { return entries_.begin(); }
      position_type end() const noexcept { return entries_.end(); }

    private:
      // Iterator handles retain retired nodes until traversal ends.
      friend void intrusive_ptr_add_ref(const OrderedTable* table) noexcept { ++table->iterators_; }
      friend void intrusive_ptr_release(const OrderedTable* table) noexcept {
        if (--table->iterators_ == 0 && std::exchange(table->retired_, false))
          std::erase_if(table->entries_, [](const Node& node) { return node.retired; });
      }

      mutable std::list<Node> entries_;
      std::unordered_map<K, position_type, OrderedHash<K>, OrderedEqual<K>> index_;
      mutable std::size_t iterators_ = 0;
      mutable bool retired_ = false;
    };

    // A live iterator over the entries of an OrderedTable, projected to a key, value or entry.
    template <class Table, class Value, auto Project = std::identity{}>
    class OrderedIterator {
    public:
      using iterator_category = std::forward_iterator_tag;
      using difference_type = std::ptrdiff_t;
      using reference = decltype(std::invoke(Project, std::declval<typename Table::entry_type&>()));
      using value_type = Value;
      using pointer = std::add_pointer_t<std::remove_reference_t<reference>>;

      OrderedIterator() = default;
      explicit OrderedIterator(const Table& table) : table_(&table), position_(table.begin()) { skipDead(); }

      reference operator*() const { return std::invoke(Project, position_->entry); }
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
      bool atEnd() const noexcept { return !table_ || position_ == table_->end(); }
      friend bool operator==(const OrderedIterator& a, const OrderedIterator& b) noexcept {
        if (a.atEnd() || b.atEnd()) return a.atEnd() && b.atEnd();
        return a.table_ == b.table_ && a.position_ == b.position_;
      }

    private:
      void skipDead() {
        while (!atEnd() && position_->retired) ++position_;
      }

      boost::intrusive_ptr<const Table> table_;
      typename Table::position_type position_{};
    };

    template <class Iterator>
    struct OrderedRange {
      Iterator first;
      Iterator begin() const { return first; }
      Iterator end() const { return Iterator(); }
    };
  }

  // new Map(): set/get/has/delete_/clear/size, for...of over [key, value] entries.
  template <class K, class V>
  class OrderedMap {
    using Entry = std::pair<const K, V>;
    using Table = detail::OrderedTable<K, Entry, &Entry::first>;

  public:
    using key_type = K;
    using mapped_type = V;
    using value_type = Entry;
    using iterator = detail::OrderedIterator<Table, std::pair<K, V>>;
    using key_iterator = detail::OrderedIterator<Table, K, &Entry::first>;
    using value_iterator = detail::OrderedIterator<Table, V, &Entry::second>;

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
      else table_.append(std::move(key), std::move(value));
      return *this;
    }
    // map.delete(key): true when an entry was removed.
    template <class Key>
    bool delete_(const Key& key) {
      return table_.erase(key);
    }
    void clear() { table_.clear(); }

    iterator begin() const { return iterator(table_); }
    iterator end() const { return iterator(); }
    detail::OrderedRange<iterator> entries() const { return {begin()}; }
    detail::OrderedRange<key_iterator> keys() const { return {key_iterator(table_)}; }
    detail::OrderedRange<value_iterator> values() const { return {value_iterator(table_)}; }
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
    using Table = detail::OrderedTable<K, const K>;

  public:
    using key_type = K;
    using value_type = K;
    using iterator = detail::OrderedIterator<Table, K>;

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
      if (!table_.find(key)) table_.append(std::move(key));
      return *this;
    }
    template <class Key>
    bool delete_(const Key& key) {
      return table_.erase(key);
    }
    void clear() { table_.clear(); }

    iterator begin() const { return iterator(table_); }
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
