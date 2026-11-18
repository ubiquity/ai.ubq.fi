#include "uos/rt/env.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <unistd.h>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <ranges>
#include <sstream>

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

    [[noreturn]] void invalidEnvFile() { throw SyntaxError("Invalid environment file"); }

    void validateEnvLines(std::string_view contents) {
      for (const auto segment : std::views::split(contents, '\n')) {
        std::string_view line(segment.begin(), segment.end());
        if (line.ends_with('\r')) line.remove_suffix(1);
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string_view::npos && line[first] == '[') invalidEnvFile();
        if (std::ranges::any_of(line, [](char byte) {
              const auto code = static_cast<unsigned char>(byte);
              return (code < 0x20 && byte != '\t') || code == 0x7f;
            })) invalidEnvFile();
      }
    }

    std::string envValue(std::string_view value) {
      if (!value.empty() && std::string_view("\"'`").find(value.front()) != std::string_view::npos) {
        std::istringstream input{std::string(value)};
        input.imbue(std::locale::classic());
        std::string quoted;
        if (!(input >> std::quoted(quoted, value.front()))) invalidEnvFile();
        input >> std::ws;
        const auto next = input.peek();
        if (next != '#' && next != std::char_traits<char>::eof()) invalidEnvFile();
        return quoted;
      }
      value = value.substr(0, value.find('#'));
      const auto end = value.find_last_not_of(" \t");
      return end == std::string_view::npos ? std::string{} : std::string(value.substr(0, end + 1));
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

  std::vector<std::pair<std::string, std::string>> parseEnvFile(std::string_view contents) {
    if (contents.size() > 256 * 1024) throw RangeError("Environment file is too large");
    if (contents.starts_with("\xEF\xBB\xBF")) contents.remove_prefix(3);
    validateEnvLines(contents);
    std::istringstream input{std::string(contents)};
    input.imbue(std::locale::classic());
    boost::property_tree::ptree tree;
    try {
      boost::property_tree::ini_parser::read_ini(input, tree);
    } catch (const boost::property_tree::ini_parser::ini_parser_error&) {
      invalidEnvFile();
    }

    std::vector<std::pair<std::string, std::string>> assignments;
    assignments.reserve(tree.size());
    for (const auto& [key, entry] : tree) {
      constexpr std::string_view keyCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-";
      if (key.empty() || key.find_first_not_of(keyCharacters) != std::string::npos) invalidEnvFile();
      assignments.emplace_back(key, envValue(entry.data()));
    }
    return assignments;
  }

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
