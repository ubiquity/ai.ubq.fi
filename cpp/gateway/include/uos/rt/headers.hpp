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
    // What a request head carries for `headers` (the HTTP client stage only; not a JS API): each
    // header except Set-Cookie, in iteration order, as {name as first given, value as sent}. The
    // name is the caller's spelling from the append/set that created the header (kept by later
    // appends and sets in any spelling, replaced only after a delete); the value is the combined
    // value get() returns (Cookie values are joined with "; " as Bun does). Set-Cookie values are getSetCookie().
    std::vector<std::pair<std::string, std::string>> headerWireFields(const Headers& headers);
  }

  // A JS Headers object. Copying is `new Headers(other)`; to alias one (JS
  // `const headers = response.headers`) hold a reference.
  class Headers {
  public:
    using Entry = std::pair<std::string, std::string>;

    // `new Headers()`
    Headers() = default;
    // `new Headers({ name: value, ... })` and `new Headers([[name, value], ...])`:
    // appends every entry in order; the first invalid name or value throws.
    Headers(std::initializer_list<Entry> init);
    explicit Headers(const std::vector<Entry>& init);
    // `new Headers(sequence)` whose items may have any length: TypeError
    // "Header sub-sequence must contain exactly two items" at the first item
    // that is not a pair (items before it are validated first).
    static Headers fromSequence(const std::vector<std::vector<std::string>>& sequence);

    // Every method validates `name` (TypeError "Invalid header name: '<name>'";
    // has() closes the quote with '"' as Bun does) and, where it takes one, the
    // value after stripping leading and trailing tab/LF/CR/space (TypeError
    // "Header '<name>' has invalid value: '<stripped value>'").
    void append(std::string_view name, std::string_view value);
    void delete_(std::string_view name);
    // `headers.get(name)`: std::nullopt where JS returns null. Set-Cookie values
    // are joined with ", "; Cookie values with "; ".
    std::optional<std::string> get(std::string_view name) const;
    std::vector<std::string> getSetCookie() const;
    bool has(std::string_view name) const;
    void set(std::string_view name, std::string_view value);
    // Bun's `headers.count`: the number of entries iteration yields.
    std::size_t count() const { return fields_.size() + cookies_.size(); }

    // `[...headers]`, `[...headers.keys()]`, `[...headers.values()]`: snapshots
    // taken now (JS iterators are live; see begin()).
    std::vector<Entry> entries() const;
    std::vector<std::string> keys() const;
    std::vector<std::string> values() const;
    // `headers.forEach((value, name) => ...)`: live, like the JS iterator.
    void forEach(const std::function<void(const std::string& value, const std::string& name)>& callback) const;

    // `for (const [name, value] of headers)`: each step reads the entry at the
    // iterator's index in the current state, so mutations made by the loop body
    // are observed exactly as in Bun.
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
    friend std::vector<Entry> detail::headerWireFields(const Headers& headers);
    Entry entryAt(std::size_t index) const;
    // Lowercased name and combined value of every header except Set-Cookie,
    // sorted by name.
    std::vector<Entry> fields_;
    std::vector<std::string> cookies_;
    // For the request head (detail::headerWireFields): fields_[i]'s name as first given, and the
    // Cookie header's value with its appended parts joined by "; ".
    std::vector<std::string> spellings_;
    std::string cookieWire_;
  };
}
