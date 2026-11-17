#include "uos/rt/env.hpp"

#include "uos/rt/fs.hpp"
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <cstring>

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

    // A canonical array index ("0" .. "4294967294"), which JS objects enumerate first.
    bool isArrayIndex(std::string_view key) {
      if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return false;
      std::uint64_t value = 0;
      for (char c : key) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
      }
      return value <= 4294967294ULL;
    }

    std::uint64_t indexValue(std::string_view key) {
      std::uint64_t value = 0;
      for (char c : key) value = value * 10 + static_cast<std::uint64_t>(c - '0');
      return value;
    }

    // ---- Bun's .env parser

    bool isKeyChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'; }
    // Around keys, line breaks are whitespace too.
    bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\n' || c == '\r'; }
    bool isInlineWhitespace(char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f'; }

    class Parser {
    public:
      explicit Parser(std::string_view source) : src(source) {}

      std::vector<std::pair<std::string, std::string>> parse() {
        std::vector<std::pair<std::string, std::string>> out;
        if (src.substr(0, 3) == "\xEF\xBB\xBF") pos = 3;
        while (pos < src.size()) {
          const auto key = parseKey(true);
          if (!key) {
            skipLine();
            continue;
          }
          std::string value = parseValue();
          auto existing = std::find_if(out.begin(), out.end(), [&](const auto& entry) { return entry.first == *key; });
          if (existing != out.end()) existing->second = std::move(value);
          else out.emplace_back(std::string(*key), std::move(value));
        }
        return out;
      }

    private:
      std::string_view src;
      std::size_t pos = 0;

      void skipLine() {
        const auto next = src.find_first_of("\n\r", pos);
        pos = next == std::string_view::npos ? src.size() : next + 1;
      }

      void skipWhitespaces() {
        while (pos < src.size() && isWhitespace(src[pos])) ++pos;
      }

      std::optional<std::string_view> parseKey(bool checkExport) {
        if (checkExport) skipWhitespaces();
        const std::size_t start = pos;
        std::size_t end = start;
        while (end < src.size() && isKeyChar(src[end])) ++end;
        if (end < src.size() && start < end) {
          pos = end;
          skipWhitespaces();
          if (pos < src.size()) {
            if (checkExport && end < pos && src.substr(start, end - start) == "export") {
              if (auto key = parseKey(false)) return key;
            }
            if (src[pos] == '=') {
              ++pos;
              return src.substr(start, end - start);
            }
            if (src[pos] == ':' && pos + 1 < src.size() && isWhitespace(src[pos + 1])) {
              pos += 2;
              return src.substr(start, end - start);
            }
          }
        }
        pos = start;
        return std::nullopt;
      }

      // A value opened by `quote` at pos: std::nullopt when it never closes.
      std::optional<std::string> parseQuoted(char quote) {
        const std::size_t start = pos;
        std::size_t end = start + 1;
        for (; end < src.size(); ++end) {
          if (src[end] == '\\') ++end;
          else if (src[end] == quote) break;
        }
        if (end >= src.size()) return std::nullopt;
        std::string out;
        for (std::size_t i = start + 1; i < end;) {
          const char c = src[i];
          if (c == '\\') {
            if (quote == '"' && i + 1 < end) {
              const char next = src[i + 1];
              if (next == 'n') out += '\n';
              else if (next == 'r') out += '\r';
              else {
                out += c;
                out += next;
              }
              i += 2;
            } else {
              out += '\\';
              ++i;
            }
          } else if (c == '\r') {
            ++i;
            if (i >= end || src[i] != '\n') out += '\n';
          } else {
            out += c;
            ++i;
          }
        }
        pos = end + 1;
        return out;
      }

      std::string parseValue() {
        while (pos < src.size() && isInlineWhitespace(src[pos])) ++pos;
        const std::size_t start = pos;
        if (pos < src.size() && (src[pos] == '"' || src[pos] == '\'' || src[pos] == '`')) {
          if (auto value = parseQuoted(src[pos])) {
            skipLine();
            return std::move(*value);
          }
        }
        std::size_t end = start;
        while (end < src.size() && src[end] != '#' && src[end] != '\r' && src[end] != '\n') ++end;
        pos = end;
        while (end > start && isInlineWhitespace(src[end - 1])) --end;
        return std::string(src.substr(start, end - start));
      }
    };
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
    std::vector<std::pair<std::string, std::string>> indexed, named;
    for (const auto& entry : current().entries) (isArrayIndex(entry.first) ? indexed : named).push_back(entry);
    std::stable_sort(indexed.begin(), indexed.end(), [](const auto& left, const auto& right) { return indexValue(left.first) < indexValue(right.first); });
    indexed.insert(indexed.end(), std::make_move_iterator(named.begin()), std::make_move_iterator(named.end()));
    return indexed;
  }

  std::vector<std::string> keys() {
    std::vector<std::string> out;
    for (auto& entry : entries()) out.push_back(std::move(entry.first));
    return out;
  }

  std::vector<std::pair<std::string, std::string>> parseEnvFile(std::string_view contents) { return Parser(contents).parse(); }

  void loadEnvFile(std::string_view path) {
    const fs::Bytes bytes = fs::readFileSync(path);
    const std::string_view contents(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    for (const auto& [name, value] : parseEnvFile(contents))
      if (!has(name)) set(name, value);
  }

  ScopedEnvironment::ScopedEnvironment(std::vector<std::pair<std::string, std::string>> variables) {
    current();
    saved_ = std::move(slot());
    auto replacement = std::make_unique<Store>();
    for (auto& [name, value] : variables) replacement->assign(name, value);
    slot() = std::move(replacement);
  }

  ScopedEnvironment::~ScopedEnvironment() { slot() = std::move(saved_); }

  ScopedOverride::ScopedOverride(std::vector<std::pair<std::string, std::optional<std::string>>> updates) {
    for (auto& [name, value] : updates) {
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
