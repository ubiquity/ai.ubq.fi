#pragma once
// Ada-backed WHATWG URL and URLSearchParams, plus the node:url POSIX adapter.
// Inputs use the runtime's WTF-8 string representation (docs/rt/text.md).
// Reference: docs/rt/web.md.
#include "uos/rt/error.hpp"
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ada {
  struct url_aggregator;
  struct url_search_params;
}

namespace uos::rt {
  // `new URL(...)` / `url.href = ...` failure: TypeError "Invalid URL" with
  // Bun's `code` and `input` properties.
  struct InvalidURLError : TypeError {
    std::string code = "ERR_INVALID_URL";
    std::string input;
    explicit InvalidURLError(std::string input_) : TypeError("Invalid URL"), input(std::move(input_)) {}
  };

  // node:url errors carry Node's error codes.
  struct NodeURLError : TypeError {
    std::string code;
    NodeURLError(std::string message_, std::string code_) : TypeError(std::move(message_)), code(std::move(code_)) {}
  };

  namespace detail {
    // A snapshot of the parsed URL fields (https://url.spec.whatwg.org/#concept-url).
    struct URLRecord {
      std::string scheme;
      std::string username;
      std::string password;
      std::optional<std::string> host;
      std::optional<std::uint16_t> port;
      bool hasOpaquePath = false;
      std::string opaquePath;
      std::vector<std::string> path;
      std::optional<std::string> query;
      std::optional<std::string> fragment;
    };
  }

  class URL;

  // A JS URLSearchParams. Copying produces a detached copy, exactly like
  // `new URLSearchParams(other)`; the object returned by URL::searchParams()
  // stays linked to its URL.
  class URLSearchParams {
  public:
    using Entry = std::pair<std::string, std::string>;

    URLSearchParams() noexcept;
    // `new URLSearchParams(string)`: one leading "?" is ignored.
    explicit URLSearchParams(std::string_view init);
    explicit URLSearchParams(const char* init) : URLSearchParams(std::string_view(init)) {}
    explicit URLSearchParams(const std::string& init) : URLSearchParams(std::string_view(init)) {}
    // `new URLSearchParams([[name, value], ...])`.
    explicit URLSearchParams(std::vector<Entry> sequence);
    URLSearchParams(std::initializer_list<Entry> sequence) : URLSearchParams(std::vector<Entry>(sequence)) {}
    // A sequence whose items are arbitrary-length arrays: each must hold exactly
    // two strings, else TypeError "Type error" (Bun's text).
    static URLSearchParams fromSequence(const std::vector<std::vector<std::string>>& sequence);
    // `new URLSearchParams({ ...record })`: JS property order, so canonical
    // array-index keys come first in ascending numeric order; a repeated key
    // keeps its first position and its last value.
    static URLSearchParams fromRecord(const std::vector<Entry>& record);

    URLSearchParams(const URLSearchParams& other);
    URLSearchParams& operator=(const URLSearchParams& other);
    URLSearchParams(URLSearchParams&& other) noexcept;
    URLSearchParams& operator=(URLSearchParams&& other) noexcept;
    ~URLSearchParams();

    std::size_t size() const;
    void append(std::string_view name, std::string_view value);
    void delete_(std::string_view name);
    void delete_(std::string_view name, std::string_view value);
    std::optional<std::string> get(std::string_view name) const;
    std::vector<std::string> getAll(std::string_view name) const;
    bool has(std::string_view name) const;
    bool has(std::string_view name, std::string_view value) const;
    void set(std::string_view name, std::string_view value);
    // Stable sort by name in UTF-16 code unit order.
    void sort();
    std::string toString() const;

    // `[...params]`, `[...params.keys()]`, `[...params.values()]` snapshots.
    std::vector<Entry> entries() const;
    std::vector<std::string> keys() const;
    std::vector<std::string> values() const;
    // `params.forEach((value, key) => ...)`: live, like the JS iterator, so
    // mutations made by the callback are observed.
    void forEach(const std::function<void(const std::string& value, const std::string& key)>& callback) const;

    // Live iteration (`for (const [key, value] of params)`): the iterator keeps
    // an index into the current list, as in JS.
    class iterator {
    public:
      iterator(const URLSearchParams* owner, std::size_t index) : owner_(owner), index_(index) {}
      Entry operator*() const;
      iterator& operator++() {
        ++index_;
        return *this;
      }
      bool operator!=(const iterator&) const { return index_ < owner_->size(); }
      bool operator==(const iterator& other) const { return !(*this != other); }

    private:
      const URLSearchParams* owner_;
      std::size_t index_;
    };
    iterator begin() const { return iterator(this, 0); }
    iterator end() const { return iterator(this, 0); }

  private:
    friend class URL;
    std::unique_ptr<ada::url_search_params> params_;
    URL* url_ = nullptr;
    ada::url_search_params& data();
    void reset(std::string_view query);
    void update();
  };

  // A JS URL. Copy construction is `new URL(other)`; the copy owns its own
  // searchParams. Getters are `x()`, setters are `x(value)`.
  class URL {
  public:
    // `new URL(url)` / `new URL(url, base)`: throws InvalidURLError.
    explicit URL(std::string_view url);
    explicit URL(const char* url) : URL(std::string_view(url)) {}
    explicit URL(const std::string& url) : URL(std::string_view(url)) {}
    URL(std::string_view url, std::string_view base);
    URL(std::string_view url, const URL& base);
    URL(const URL& other);
    URL(URL&& other) noexcept;
    URL& operator=(const URL& other);
    URL& operator=(URL&& other) noexcept;
    ~URL();

    static bool canParse(std::string_view url);
    static bool canParse(std::string_view url, std::string_view base);
    // `URL.parse(url, base?)`: std::nullopt where JS returns null.
    static std::optional<URL> parse(std::string_view url);
    static std::optional<URL> parse(std::string_view url, std::string_view base);

    std::string href() const;
    void href(std::string_view value);
    std::string origin() const;
    std::string protocol() const;
    void protocol(std::string_view value);
    std::string username() const;
    void username(std::string_view value);
    std::string password() const;
    void password(std::string_view value);
    std::string host() const;
    void host(std::string_view value);
    std::string hostname() const;
    void hostname(std::string_view value);
    std::string port() const;
    void port(std::string_view value);
    std::string pathname() const;
    void pathname(std::string_view value);
    std::string search() const;
    void search(std::string_view value);
    std::string hash() const;
    void hash(std::string_view value);
    URLSearchParams& searchParams() { return searchParams_; }
    const URLSearchParams& searchParams() const { return searchParams_; }
    std::string toString() const { return href(); }
    std::string toJSON() const { return href(); }

    const detail::URLRecord& record() const;

  private:
    friend class URLSearchParams;
    std::unique_ptr<ada::url_aggregator> url_;
    URLSearchParams searchParams_;
    mutable std::unique_ptr<detail::URLRecord> record_;
    explicit URL(ada::url_aggregator parsed);
    void adopt(ada::url_aggregator parsed);
    void setComponent(std::string_view value, bool (ada::url_aggregator::*setter)(std::string_view), bool checkHost = false);
    void setQuery(std::string_view query);
    void refreshRecord();
  };

  // node:url pathToFileURL(path): resolves `path` against the process working
  // directory like path.resolve, keeps a trailing "/", and percent-encodes it.
  URL pathToFileURL(std::string_view path);
  // node:url fileURLToPath(url) for POSIX hosts. Throws NodeURLError.
  std::string fileURLToPath(const URL& url);
  std::string fileURLToPath(std::string_view url);
}
