#pragma once
// Fetch `Headers` as Bun 1.4 implements it (WebKit FetchHeaders): names are
// stored lowercased, `append` joins Cookie with "; " and other values with ", ",
// Set-Cookie values stay a
// separate list, and iteration yields the other names in code point order
// followed by every Set-Cookie value. Strings are WTF-8 (docs/rt/text.md); a
// stored value holds only code points up to U+00FF. Reference: docs/rt/web.md.
#include "uos/rt/error.hpp"
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::rt {
  class Headers;
  namespace detail {
    // Ordinary fields in iteration order, with the first spelling and combined value.
    // Set-Cookie lines come from getSetCookie().
    std::vector<std::pair<std::string, std::string>> headerWireFields(const Headers& headers);
  }

  class Headers {
  public:
    using Entry = std::pair<std::string, std::string>;

    Headers() = default;
    Headers(std::initializer_list<Entry> init);
    explicit Headers(const std::vector<Entry>& init);
    static Headers fromSequence(const std::vector<std::vector<std::string>>& sequence);

    // Invalid names and values throw TypeError before mutation.
    void append(std::string_view name, std::string_view value);
    void delete_(std::string_view name);
    std::optional<std::string> get(std::string_view name) const;
    std::vector<std::string> getSetCookie() const;
    bool has(std::string_view name) const;
    void set(std::string_view name, std::string_view value);
    std::size_t count() const { return fields_.size() + cookies_.size(); }

    std::vector<Entry> entries() const;
    std::vector<std::string> keys() const;
    std::vector<std::string> values() const;
    void forEach(const std::function<void(const std::string& value, const std::string& name)>& callback) const;

    // Each step reads the current entry at its index, including mutations during traversal.
    class iterator {
    public:
      iterator(const Headers* owner, std::size_t index) : owner_(owner), index_(index) {}
      Entry operator*() const { return owner_->entryAt(index_); }
      iterator& operator++() {
        ++index_;
        return *this;
      }
      bool operator!=(const iterator&) const { return index_ < owner_->count(); }
      bool operator==(const iterator& other) const { return !(*this != other); }

    private:
      const Headers* owner_;
      std::size_t index_;
    };
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, 0); }

  private:
    struct Field {
      std::string name;
      std::string spelling;
      std::string value;
    };

    friend std::vector<Entry> detail::headerWireFields(const Headers& headers);
    static auto findField(auto& fields, std::string_view key);
    void write(std::string_view name, std::string_view value, bool append);
    Entry entryAt(std::size_t index) const;
    std::vector<Field> fields_;  // Sorted by lowercase name.
    std::vector<std::string> cookies_;
  };
}
